use super::{check_range, BlockDevice};
use crate::Errno;
use std::sync::{Arc, Mutex};

/// Turns arbitrary byte requests into sector-aligned requests on `inner`
/// using read-modify-write. The mutex serializes all requests so concurrent
/// unaligned writes sharing a sector cannot lose each other's bytes.
pub struct AlignedDevice {
    inner: Arc<dyn BlockDevice>,
    lock: Mutex<()>,
}

impl AlignedDevice {
    pub fn new(inner: Arc<dyn BlockDevice>) -> AlignedDevice {
        AlignedDevice {
            inner,
            lock: Mutex::new(()),
        }
    }

    /// Aligned span [start, end) covering [off, off+len). Requires in-range;
    /// `EINVAL` if rounding up to a sector (or the span's size) overflows.
    fn span(&self, off: u64, len: usize) -> Result<(u64, u64), Errno> {
        let s = self.inner.sector_size() as u64;
        let end = off.checked_add(len as u64).ok_or(Errno::EINVAL)?;
        let end = end.div_ceil(s).checked_mul(s).ok_or(Errno::EINVAL)?;
        let start = off / s * s;
        usize::try_from(end - start).map_err(|_| Errno::EINVAL)?;
        Ok((start, end))
    }
}

impl BlockDevice for AlignedDevice {
    fn size(&self) -> u64 {
        self.inner.size()
    }
    fn sector_size(&self) -> u32 {
        self.inner.sector_size()
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        if !check_range(self.size(), off, buf.len())? {
            return Ok(());
        }
        let _g = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let (start, end) = self.span(off, buf.len())?;
        if end > self.size() {
            return Err(Errno::EINVAL); // size not a multiple of sector size
        }
        let mut tmp = vec![0u8; (end - start) as usize];
        self.inner.read_at(start, &mut tmp)?;
        let o = (off - start) as usize;
        buf.copy_from_slice(&tmp[o..o + buf.len()]);
        Ok(())
    }
    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), Errno> {
        if self.read_only() {
            return Err(Errno::EROFS);
        }
        if !check_range(self.size(), off, buf.len())? {
            return Ok(());
        }
        let _g = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let (start, end) = self.span(off, buf.len())?;
        if end > self.size() {
            return Err(Errno::EINVAL);
        }
        let mut tmp = vec![0u8; (end - start) as usize];
        self.inner.read_at(start, &mut tmp)?;
        let o = (off - start) as usize;
        tmp[o..o + buf.len()].copy_from_slice(buf);
        self.inner.write_at(start, &tmp)
    }
    fn flush(&self) -> Result<(), Errno> {
        self.inner.flush()
    }
    fn read_only(&self) -> bool {
        self.inner.read_only()
    }
}
