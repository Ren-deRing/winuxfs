use super::{check_range, BlockDevice};
use crate::Errno;
use std::fs::{File, OpenOptions};
use std::path::Path;

pub struct FileDevice {
    file: File,
    size: u64,
    read_only: bool,
}

impl FileDevice {
    pub fn open(path: &Path, read_only: bool) -> Result<FileDevice, Errno> {
        let file = OpenOptions::new()
            .read(true)
            .write(!read_only)
            .open(path)
            .map_err(|e| io_errno(&e))?;
        let size = file.metadata().map_err(|e| io_errno(&e))?.len();
        Ok(FileDevice { file, size, read_only })
    }
}

fn io_errno(e: &std::io::Error) -> Errno {
    use std::io::ErrorKind::*;
    match e.kind() {
        NotFound => Errno::ENOENT,
        _ => Errno::EIO,
    }
}

#[cfg(unix)]
fn pread(f: &File, buf: &mut [u8], off: u64) -> std::io::Result<()> {
    std::os::unix::fs::FileExt::read_exact_at(f, buf, off)
}
#[cfg(unix)]
fn pwrite(f: &File, buf: &[u8], off: u64) -> std::io::Result<()> {
    std::os::unix::fs::FileExt::write_all_at(f, buf, off)
}

// Windows seek_read/seek_write move the file cursor but take an explicit
// offset each call, so they are safe for concurrent positional use.
#[cfg(windows)]
fn pread(f: &File, mut buf: &mut [u8], mut off: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        let n = f.seek_read(buf, off)?;
        if n == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        buf = &mut buf[n..];
        off += n as u64;
    }
    Ok(())
}
#[cfg(windows)]
fn pwrite(f: &File, mut buf: &[u8], mut off: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        let n = f.seek_write(buf, off)?;
        if n == 0 {
            return Err(std::io::ErrorKind::WriteZero.into());
        }
        buf = &buf[n..];
        off += n as u64;
    }
    Ok(())
}

impl BlockDevice for FileDevice {
    fn size(&self) -> u64 {
        self.size
    }
    fn sector_size(&self) -> u32 {
        512
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        if !check_range(self.size, off, buf.len())? {
            return Ok(());
        }
        pread(&self.file, buf, off).map_err(|_| Errno::EIO)
    }
    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), Errno> {
        if self.read_only {
            return Err(Errno::EROFS);
        }
        if !check_range(self.size, off, buf.len())? {
            return Ok(());
        }
        pwrite(&self.file, buf, off).map_err(|_| Errno::EIO)
    }
    fn flush(&self) -> Result<(), Errno> {
        if self.read_only {
            return Ok(());
        }
        self.file.sync_all().map_err(|_| Errno::EIO)
    }
    fn read_only(&self) -> bool {
        self.read_only
    }
}
