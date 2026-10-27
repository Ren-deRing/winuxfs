mod aligned;
mod file;
mod mem;

pub use aligned::AlignedDevice;
pub use file::FileDevice;
pub use mem::MemDevice;

use crate::Errno;

/// Byte-addressed block device. Out-of-range requests (including `off + len`
/// overflow) return `EINVAL`; zero-length requests always succeed; writes to a
/// read-only device return `EROFS`.
pub trait BlockDevice: Send + Sync {
    fn size(&self) -> u64;
    fn sector_size(&self) -> u32;
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno>;
    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), Errno>;
    fn flush(&self) -> Result<(), Errno>;
    fn read_only(&self) -> bool;
}

/// Returns Ok(true) if the request is non-empty and in range, Ok(false) if empty.
pub(crate) fn check_range(size: u64, off: u64, len: usize) -> Result<bool, Errno> {
    if len == 0 {
        return Ok(false);
    }
    match off.checked_add(len as u64) {
        Some(end) if end <= size => Ok(true),
        _ => Err(Errno::EINVAL),
    }
}
