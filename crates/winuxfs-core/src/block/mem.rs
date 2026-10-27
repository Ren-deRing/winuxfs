use super::{check_range, BlockDevice};
use crate::Errno;
use std::sync::RwLock;

pub struct MemDevice {
    data: RwLock<Vec<u8>>,
    sector: u32,
    strict: bool,
}

impl MemDevice {
    pub fn new(size: u64, sector: u32) -> MemDevice {
        MemDevice {
            data: RwLock::new(vec![0; size as usize]),
            sector,
            strict: false,
        }
    }

    /// Rejects requests not aligned to the sector size with `EINVAL`.
    pub fn strict(size: u64, sector: u32) -> MemDevice {
        MemDevice {
            strict: true,
            ..MemDevice::new(size, sector)
        }
    }

    fn aligned(&self, off: u64, len: usize) -> bool {
        let s = self.sector as u64;
        !self.strict || (off.is_multiple_of(s) && (len as u64).is_multiple_of(s))
    }
}

impl BlockDevice for MemDevice {
    fn size(&self) -> u64 {
        self.data.read().unwrap().len() as u64
    }
    fn sector_size(&self) -> u32 {
        self.sector
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        let d = self.data.read().unwrap();
        if !check_range(d.len() as u64, off, buf.len())? {
            return Ok(());
        }
        if !self.aligned(off, buf.len()) {
            return Err(Errno::EINVAL);
        }
        buf.copy_from_slice(&d[off as usize..off as usize + buf.len()]);
        Ok(())
    }
    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), Errno> {
        let mut d = self.data.write().unwrap();
        if !check_range(d.len() as u64, off, buf.len())? {
            return Ok(());
        }
        if !self.aligned(off, buf.len()) {
            return Err(Errno::EINVAL);
        }
        d[off as usize..off as usize + buf.len()].copy_from_slice(buf);
        Ok(())
    }
    fn flush(&self) -> Result<(), Errno> {
        Ok(())
    }
    fn read_only(&self) -> bool {
        false
    }
}
