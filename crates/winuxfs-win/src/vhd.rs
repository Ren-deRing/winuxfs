// SPDX-License-Identifier: GPL-2.0-only
//! VHD/VHDX files attached through the Virtual Disk API and opened as
//! physical disks.

use std::path::Path;
use std::ptr;
use std::sync::Arc;
use std::time::{Duration, Instant};

use windows_sys::Win32::Storage::Vhd::{
    AttachVirtualDisk, DetachVirtualDisk, GetVirtualDiskPhysicalPath, OpenVirtualDisk,
    ATTACH_VIRTUAL_DISK_FLAG, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
    ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY, ATTACH_VIRTUAL_DISK_PARAMETERS,
    ATTACH_VIRTUAL_DISK_VERSION_1, DETACH_VIRTUAL_DISK_FLAG_NONE, OPEN_VIRTUAL_DISK_FLAG_NONE,
    OPEN_VIRTUAL_DISK_PARAMETERS, OPEN_VIRTUAL_DISK_VERSION_2, VIRTUAL_DISK_ACCESS_NONE,
    VIRTUAL_STORAGE_TYPE,
};
use winuxfs_core::{BlockDevice, Errno};

use crate::windisk::{wide, win32_errno, Handle, WinDisk};
use crate::{parse_device_spec, DeviceSpec};

/// How long to wait for the attached disk's device to accept an open.
const SURFACE_TIMEOUT: Duration = Duration::from_secs(10);

/// An attached virtual disk and the block device on it. Dropping it closes
/// the device and detaches the disk.
///
/// The attachment is not permanent: Windows ties it to the virtual disk
/// handle held here, so even if the process is killed the disk is detached
/// when the kernel closes the handle.
pub struct VhdAttachment {
    device: Option<Arc<dyn BlockDevice>>,
    number: u32,
    vhd: Handle,
}

impl VhdAttachment {
    /// The attached disk (unaligned requests are allowed). Clones that
    /// outlive the attachment fail with I/O errors after the detach.
    pub fn device(&self) -> Arc<dyn BlockDevice> {
        Arc::clone(self.device.as_ref().expect("device present until drop"))
    }

    /// The `N` of the attached disk's `\\.\PhysicalDriveN`.
    pub fn disk_number(&self) -> u32 {
        self.number
    }
}

impl Drop for VhdAttachment {
    fn drop(&mut self) {
        // Close our device first (restoring its online state), then detach.
        // Errors are ignored; closing `vhd` afterwards detaches as well.
        drop(self.device.take());
        unsafe { DetachVirtualDisk(self.vhd.0, DETACH_VIRTUAL_DISK_FLAG_NONE, 0) };
    }
}

/// Attachment flags: no drive letters for the disk's volumes, read-only
/// when asked. `ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST` is deliberately not
/// used: it keeps the disk off the local host, so there is no
/// `PhysicalDriveN` to open (`GetVirtualDiskPhysicalPath` then fails with
/// `ERROR_DEV_NOT_EXIST`, checked on Windows 11 build 26300).
/// `ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME` is not used either, so the
/// attachment ends with the handle.
fn attach_flags(read_only: bool) -> ATTACH_VIRTUAL_DISK_FLAG {
    let mut f = ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
    if read_only {
        f |= ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY;
    }
    f
}

/// Attaches the VHD or VHDX at `path` and opens its disk with
/// [`WinDisk::open`]. Needs an administrator token (`EACCES` otherwise);
/// a missing file is `ENOENT`.
pub fn attach_vhd(path: &Path, read_only: bool) -> Result<VhdAttachment, Errno> {
    let wpath = wide(path.to_str().ok_or(Errno::EINVAL)?);
    // DeviceId/VendorId "unknown" (zero): the type is taken from the file.
    let storage: VIRTUAL_STORAGE_TYPE = unsafe { std::mem::zeroed() };
    let mut open = OPEN_VIRTUAL_DISK_PARAMETERS {
        Version: OPEN_VIRTUAL_DISK_VERSION_2,
        ..Default::default()
    };
    // VHDX requires version 2 parameters with access mask NONE.
    open.Anonymous.Version2.ReadOnly = read_only.into();
    let mut h = ptr::null_mut();
    let rc = unsafe {
        OpenVirtualDisk(
            &storage,
            wpath.as_ptr(),
            VIRTUAL_DISK_ACCESS_NONE,
            OPEN_VIRTUAL_DISK_FLAG_NONE,
            &open,
            &mut h,
        )
    };
    if rc != 0 {
        return Err(win32_errno(rc));
    }
    let vhd = Handle(h);

    let attach = ATTACH_VIRTUAL_DISK_PARAMETERS {
        Version: ATTACH_VIRTUAL_DISK_VERSION_1,
        ..Default::default()
    };
    let rc = unsafe {
        AttachVirtualDisk(
            vhd.0,
            ptr::null_mut(),
            attach_flags(read_only),
            0,
            &attach,
            ptr::null(),
        )
    };
    if rc != 0 {
        return Err(win32_errno(rc));
    }
    // From here on dropping `att` detaches.
    let mut att = VhdAttachment {
        device: None,
        number: 0,
        vhd,
    };

    let mut buf = [0u16; 512];
    let mut bytes = (buf.len() * 2) as u32;
    let rc = unsafe { GetVirtualDiskPhysicalPath(att.vhd.0, &mut bytes, buf.as_mut_ptr()) };
    if rc != 0 {
        return Err(win32_errno(rc));
    }
    let len = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    let phys = String::from_utf16_lossy(&buf[..len]);
    att.number = match parse_device_spec(&phys) {
        DeviceSpec::PhysicalDrive(n) => n,
        _ => return Err(Errno::EIO),
    };

    // The disk device can lag the attach call slightly.
    let start = Instant::now();
    let dev = loop {
        match WinDisk::open(att.number, read_only) {
            Err(Errno::ENOENT) if start.elapsed() < SURFACE_TIMEOUT => {
                std::thread::sleep(Duration::from_millis(100))
            }
            r => break r?,
        }
    };
    att.device = Some(dev);
    Ok(att)
}
