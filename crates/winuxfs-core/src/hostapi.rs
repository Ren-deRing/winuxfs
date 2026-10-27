//! The `wfs_host_api` table handed to backend plugins.
//!
//! A `wfs_blkdev *` is a pointer to a heap-allocated `Arc<dyn BlockDevice>`
//! owned by a [`BlkHandle`]. Every `extern "C"` function here checks its
//! pointer arguments for NULL and runs its body under `catch_unwind`, so a
//! panic never unwinds into C (it becomes `-EIO`, or 0 for the size getters).

use crate::{BlockDevice, Errno};
use std::ffi::{c_char, c_int, c_void, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Arc;
use winuxfs_abi::{wfs_blkdev, wfs_err_t, wfs_host_api, WFS_ABI_VERSION};

/// Owns one block device as seen by C. The pointer from [`BlkHandle::as_ptr`]
/// stays valid for as long as the handle lives.
pub struct BlkHandle(Box<Arc<dyn BlockDevice>>);

impl BlkHandle {
    pub fn new(dev: Arc<dyn BlockDevice>) -> BlkHandle {
        BlkHandle(Box::new(dev))
    }

    pub fn as_ptr(&self) -> *mut wfs_blkdev {
        (&*self.0 as *const Arc<dyn BlockDevice>).cast_mut().cast()
    }

    pub fn device(&self) -> &Arc<dyn BlockDevice> {
        &self.0
    }
}

static HOST_API: wfs_host_api = wfs_host_api {
    abi_version: WFS_ABI_VERSION,
    size: std::mem::size_of::<wfs_host_api>() as u32,
    blk_size: Some(blk_size),
    blk_sector_size: Some(blk_sector_size),
    blk_read_only: Some(blk_read_only),
    blk_read: Some(blk_read),
    blk_write: Some(blk_write),
    blk_flush: Some(blk_flush),
    log: Some(log),
};

/// The process-wide host API table (static, so plugins may keep the pointer).
pub fn host_api() -> &'static wfs_host_api {
    &HOST_API
}

fn neg(e: Errno) -> wfs_err_t {
    -e.0
}

fn status(r: Result<(), Errno>) -> wfs_err_t {
    match r {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

/// Runs `f` with the device behind `d`; `null` is returned for a NULL device,
/// `panicked` if `f` panics.
///
/// # Safety
/// `d` must be NULL or a pointer obtained from a live [`BlkHandle`].
unsafe fn with_dev<R>(
    d: *mut wfs_blkdev,
    null: R,
    panicked: R,
    f: impl FnOnce(&dyn BlockDevice) -> R,
) -> R {
    if d.is_null() {
        return null;
    }
    let dev: &Arc<dyn BlockDevice> = &*(d as *const Arc<dyn BlockDevice>);
    catch_unwind(AssertUnwindSafe(|| f(&**dev))).unwrap_or(panicked)
}

unsafe extern "C" fn blk_size(d: *mut wfs_blkdev) -> u64 {
    with_dev(d, 0, 0, |dev| dev.size())
}

unsafe extern "C" fn blk_sector_size(d: *mut wfs_blkdev) -> u32 {
    with_dev(d, 0, 0, |dev| dev.sector_size())
}

/// NULL or a panicking device reports read-only (the safe answer).
unsafe extern "C" fn blk_read_only(d: *mut wfs_blkdev) -> c_int {
    with_dev(d, 1, 1, |dev| dev.read_only() as c_int)
}

unsafe extern "C" fn blk_read(
    d: *mut wfs_blkdev,
    off: u64,
    buf: *mut c_void,
    len: usize,
) -> wfs_err_t {
    let einval = neg(Errno::EINVAL);
    with_dev(d, einval, neg(Errno::EIO), |dev| {
        if len == 0 {
            return status(dev.read_at(off, &mut []));
        }
        if buf.is_null() || len > isize::MAX as usize {
            return einval;
        }
        let b = std::slice::from_raw_parts_mut(buf.cast::<u8>(), len);
        status(dev.read_at(off, b))
    })
}

/// Any write to a read-only device, including length 0, is `-EROFS`.
unsafe extern "C" fn blk_write(
    d: *mut wfs_blkdev,
    off: u64,
    buf: *const c_void,
    len: usize,
) -> wfs_err_t {
    let einval = neg(Errno::EINVAL);
    with_dev(d, einval, neg(Errno::EIO), |dev| {
        if dev.read_only() {
            return neg(Errno::EROFS);
        }
        if len == 0 {
            return status(dev.write_at(off, &[]));
        }
        if buf.is_null() || len > isize::MAX as usize {
            return einval;
        }
        let b = std::slice::from_raw_parts(buf.cast::<u8>(), len);
        status(dev.write_at(off, b))
    })
}

unsafe extern "C" fn blk_flush(d: *mut wfs_blkdev) -> wfs_err_t {
    with_dev(d, neg(Errno::EINVAL), neg(Errno::EIO), |dev| {
        status(dev.flush())
    })
}

/// Levels 0 (error) and 1 (warn) always go to stderr; 2 (info) and 3 (debug)
/// only when `WINUXFS_LOG` is set. A NULL message is ignored.
unsafe extern "C" fn log(level: c_int, msg: *const c_char) {
    if msg.is_null() {
        return;
    }
    let _ = catch_unwind(|| {
        if level > 1 && std::env::var_os("WINUXFS_LOG").is_none() {
            return;
        }
        let tag = match level {
            0 => "error",
            1 => "warn",
            2 => "info",
            _ => "debug",
        };
        let m = CStr::from_ptr(msg).to_string_lossy();
        eprintln!("[backend {tag}] {m}");
    });
}
