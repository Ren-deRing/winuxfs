// SPDX-License-Identifier: GPL-2.0-only
//! Physical disks (`\\.\PhysicalDriveN`) as block devices.
//!
//! Opening a physical drive needs an elevated (administrator) token; without
//! one `CreateFileW` fails with `ERROR_ACCESS_DENIED`, reported as `EACCES`.

use std::alloc::{self, Layout};
use std::ptr;
use std::sync::Arc;

use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_ACCESS_DENIED, ERROR_BUSY, ERROR_DEV_NOT_EXIST,
    ERROR_FILE_NOT_FOUND, ERROR_INVALID_PARAMETER, ERROR_LOCK_VIOLATION, ERROR_PATH_NOT_FOUND,
    ERROR_SHARING_VIOLATION, ERROR_WRITE_PROTECT, GENERIC_READ, GENERIC_WRITE, HANDLE,
    INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FlushFileBuffers, ReadFile, WriteFile, FILE_FLAG_NO_BUFFERING,
    FILE_FLAG_WRITE_THROUGH, FILE_SHARE_READ, FILE_SHARE_WRITE,
    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, OPEN_EXISTING,
};
use windows_sys::Win32::System::Ioctl::{
    DISK_ATTRIBUTE_OFFLINE, DISK_EXTENT, DISK_GEOMETRY_EX, GET_DISK_ATTRIBUTES,
    IOCTL_DISK_GET_DISK_ATTRIBUTES, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
    IOCTL_DISK_SET_DISK_ATTRIBUTES, SET_DISK_ATTRIBUTES, VOLUME_DISK_EXTENTS,
};
use windows_sys::Win32::System::SystemInformation::GetWindowsDirectoryW;
use windows_sys::Win32::System::IO::{DeviceIoControl, OVERLAPPED};
use winuxfs_core::{AlignedDevice, BlockDevice, Errno};

/// Largest single ReadFile/WriteFile; also the bounce buffer size bound.
const MAX_IO: usize = 1 << 20;
/// Memory alignment of bounce buffers (unbuffered I/O needs sector-aligned
/// memory; a page is aligned for every sector size in use).
const BUF_ALIGN: usize = 4096;

/// Maps a Win32 error code to an errno.
pub(crate) fn win32_errno(code: u32) -> Errno {
    match code {
        ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND => Errno::ENOENT,
        ERROR_ACCESS_DENIED => Errno::EACCES,
        ERROR_SHARING_VIOLATION | ERROR_LOCK_VIOLATION | ERROR_BUSY => Errno::EBUSY,
        ERROR_WRITE_PROTECT => Errno::EROFS,
        ERROR_INVALID_PARAMETER => Errno::EINVAL,
        ERROR_DEV_NOT_EXIST => Errno::ENODEV,
        _ => Errno::EIO,
    }
}

fn last_errno() -> Errno {
    win32_errno(unsafe { GetLastError() })
}

pub(crate) fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Owned Win32 handle, closed on drop.
pub(crate) struct Handle(pub(crate) HANDLE);

// A kernel handle may be used from any thread; all I/O on it is positional.
unsafe impl Send for Handle {}
unsafe impl Sync for Handle {}

impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0) };
    }
}

fn open_path(path: &str, access: u32, flags: u32) -> Result<Handle, Errno> {
    let w = wide(path);
    let h = unsafe {
        CreateFileW(
            w.as_ptr(),
            access,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            ptr::null(),
            OPEN_EXISTING,
            flags,
            ptr::null_mut(),
        )
    };
    if h == INVALID_HANDLE_VALUE {
        Err(last_errno())
    } else {
        Ok(Handle(h))
    }
}

/// `DeviceIoControl` with a fixed-size input and output; returns bytes out.
fn ioctl<I, O>(
    h: &Handle,
    code: u32,
    input: Option<&I>,
    out: Option<&mut O>,
) -> Result<u32, Errno> {
    let (ip, il) = match input {
        Some(i) => (i as *const I as *const _, size_of::<I>() as u32),
        None => (ptr::null(), 0),
    };
    let (op, ol) = match out {
        Some(o) => (o as *mut O as *mut _, size_of::<O>() as u32),
        None => (ptr::null_mut(), 0),
    };
    let mut got = 0u32;
    let ok = unsafe { DeviceIoControl(h.0, code, ip, il, op, ol, &mut got, ptr::null_mut()) };
    if ok == 0 {
        Err(last_errno())
    } else {
        Ok(got)
    }
}

/// Numbers of the disks that hold the volume of the Windows directory.
fn system_disk_numbers() -> Result<Vec<u32>, Errno> {
    let mut dir = [0u16; 261];
    let n = unsafe { GetWindowsDirectoryW(dir.as_mut_ptr(), dir.len() as u32) } as usize;
    if n < 2 || n >= dir.len() || dir[1] != u16::from(b':') {
        return Err(Errno::EIO);
    }
    let letter = char::from_u32(u32::from(dir[0])).ok_or(Errno::EIO)?;
    let vol = open_path(&format!(r"\\.\{letter}:"), 0, 0)?;
    // Room for 32 extents (a spanned system volume is already exotic).
    #[repr(C)]
    struct Extents {
        head: VOLUME_DISK_EXTENTS,
        more: [DISK_EXTENT; 31],
    }
    let mut ext: Extents = unsafe { std::mem::zeroed() };
    ioctl::<(), _>(
        &vol,
        IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        None,
        Some(&mut ext),
    )?;
    let count = (ext.head.NumberOfDiskExtents as usize).min(32);
    let base = ptr::addr_of!(ext.head.Extents) as *const DISK_EXTENT;
    Ok((0..count)
        .map(|i| unsafe { (*base.add(i)).DiskNumber })
        .collect())
}

fn set_offline(h: &Handle, offline: bool) -> Result<(), Errno> {
    let attrs = SET_DISK_ATTRIBUTES {
        Version: size_of::<SET_DISK_ATTRIBUTES>() as u32,
        // Not persisted: if the process dies before restoring, a reboot
        // brings the disk back online.
        Persist: false,
        Attributes: if offline { DISK_ATTRIBUTE_OFFLINE } else { 0 },
        AttributesMask: DISK_ATTRIBUTE_OFFLINE,
        ..Default::default()
    };
    ioctl::<_, ()>(h, IOCTL_DISK_SET_DISK_ATTRIBUTES, Some(&attrs), None).map(|_| ())
}

/// Opens physical disks.
pub struct WinDisk;

impl WinDisk {
    /// Opens `\\.\PhysicalDrive<number>`. The result accepts unaligned
    /// requests (it is wrapped in [`AlignedDevice`]).
    ///
    /// In write mode the disk is first taken offline (not persistently), so
    /// Windows dismounts its volumes and leaves it alone; when the last
    /// reference to the device is dropped the disk is brought back online,
    /// but only if this call took it offline. Write mode is refused with
    /// `EBUSY` for the disk that holds the Windows system volume. Without an
    /// administrator token the open fails with `EACCES`.
    pub fn open(number: u32, read_only: bool) -> Result<Arc<dyn BlockDevice>, Errno> {
        if !read_only && system_disk_numbers()?.contains(&number) {
            return Err(Errno::EBUSY);
        }
        let path = format!(r"\\.\PhysicalDrive{number}");
        let (access, flags) = if read_only {
            (GENERIC_READ, FILE_FLAG_NO_BUFFERING)
        } else {
            (
                GENERIC_READ | GENERIC_WRITE,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
            )
        };
        let handle = open_path(&path, access, flags)?;

        #[repr(C)]
        struct Geometry {
            g: DISK_GEOMETRY_EX,
            spare: [u8; 256],
        }
        let mut geo: Geometry = unsafe { std::mem::zeroed() };
        ioctl::<(), _>(
            &handle,
            IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            None,
            Some(&mut geo),
        )?;
        let sector = geo.g.Geometry.BytesPerSector;
        let size = u64::try_from(geo.g.DiskSize).map_err(|_| Errno::EIO)?;
        if sector == 0 || !sector.is_power_of_two() || sector as usize > BUF_ALIGN {
            return Err(Errno::EIO);
        }

        let mut disk = RawDisk {
            handle,
            size,
            sector,
            read_only,
            restore_online: false,
        };
        if !read_only {
            let mut cur = GET_DISK_ATTRIBUTES::default();
            ioctl::<(), _>(
                &disk.handle,
                IOCTL_DISK_GET_DISK_ATTRIBUTES,
                None,
                Some(&mut cur),
            )?;
            if cur.Attributes & DISK_ATTRIBUTE_OFFLINE == 0 {
                set_offline(&disk.handle, true)?;
                disk.restore_online = true;
            }
        }
        Ok(Arc::new(AlignedDevice::new(Arc::new(disk))))
    }
}

/// Sector-aligned access to an open physical drive.
struct RawDisk {
    handle: Handle,
    size: u64,
    sector: u32,
    read_only: bool,
    /// Set when `open` took the disk offline; cleared once restored.
    restore_online: bool,
}

impl Drop for RawDisk {
    fn drop(&mut self) {
        // Runs once per RawDisk; errors are ignored (nothing to report to,
        // and a drop must not panic). The handle closes afterwards.
        if std::mem::take(&mut self.restore_online) {
            let _ = set_offline(&self.handle, false);
        }
    }
}

/// Page-aligned heap buffer for unbuffered I/O.
struct AlignedBuf {
    ptr: *mut u8,
    layout: Layout,
}

impl AlignedBuf {
    fn new(len: usize) -> AlignedBuf {
        let layout = Layout::from_size_align(len.max(1), BUF_ALIGN).expect("layout");
        let ptr = unsafe { alloc::alloc_zeroed(layout) };
        if ptr.is_null() {
            alloc::handle_alloc_error(layout);
        }
        AlignedBuf { ptr, layout }
    }
    fn slice(&mut self, len: usize) -> &mut [u8] {
        assert!(len <= self.layout.size());
        unsafe { std::slice::from_raw_parts_mut(self.ptr, len) }
    }
}

impl Drop for AlignedBuf {
    fn drop(&mut self) {
        unsafe { alloc::dealloc(self.ptr, self.layout) };
    }
}

fn overlapped_at(off: u64) -> OVERLAPPED {
    let mut ov: OVERLAPPED = unsafe { std::mem::zeroed() };
    ov.Anonymous.Anonymous.Offset = off as u32;
    ov.Anonymous.Anonymous.OffsetHigh = (off >> 32) as u32;
    ov
}

impl RawDisk {
    fn check(&self, off: u64, len: usize) -> Result<bool, Errno> {
        if len == 0 {
            return Ok(false);
        }
        let s = u64::from(self.sector);
        match off.checked_add(len as u64) {
            Some(end)
                if end <= self.size && off.is_multiple_of(s) && (len as u64).is_multiple_of(s) =>
            {
                Ok(true)
            }
            _ => Err(Errno::EINVAL),
        }
    }
}

impl BlockDevice for RawDisk {
    fn size(&self) -> u64 {
        self.size
    }
    fn sector_size(&self) -> u32 {
        self.sector
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        if !self.check(off, buf.len())? {
            return Ok(());
        }
        let mut bounce = AlignedBuf::new(buf.len().min(MAX_IO));
        for (i, chunk) in buf.chunks_mut(MAX_IO).enumerate() {
            let tmp = bounce.slice(chunk.len());
            let ov = overlapped_at(off + (i * MAX_IO) as u64);
            let mut got = 0u32;
            let ok = unsafe {
                ReadFile(
                    self.handle.0,
                    tmp.as_mut_ptr(),
                    tmp.len() as u32,
                    &mut got,
                    &ov as *const _ as *mut _,
                )
            };
            if ok == 0 {
                return Err(last_errno());
            }
            if got as usize != tmp.len() {
                return Err(Errno::EIO);
            }
            chunk.copy_from_slice(tmp);
        }
        Ok(())
    }
    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), Errno> {
        if self.read_only {
            return Err(Errno::EROFS);
        }
        if !self.check(off, buf.len())? {
            return Ok(());
        }
        let mut bounce = AlignedBuf::new(buf.len().min(MAX_IO));
        for (i, chunk) in buf.chunks(MAX_IO).enumerate() {
            let tmp = bounce.slice(chunk.len());
            tmp.copy_from_slice(chunk);
            let ov = overlapped_at(off + (i * MAX_IO) as u64);
            let mut put = 0u32;
            let ok = unsafe {
                WriteFile(
                    self.handle.0,
                    tmp.as_ptr(),
                    tmp.len() as u32,
                    &mut put,
                    &ov as *const _ as *mut _,
                )
            };
            if ok == 0 {
                return Err(last_errno());
            }
            if put as usize != tmp.len() {
                return Err(Errno::EIO);
            }
        }
        Ok(())
    }
    fn flush(&self) -> Result<(), Errno> {
        if self.read_only {
            return Ok(());
        }
        if unsafe { FlushFileBuffers(self.handle.0) } == 0 {
            return Err(last_errno());
        }
        Ok(())
    }
    fn read_only(&self) -> bool {
        self.read_only
    }
}
