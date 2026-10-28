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
    /// Opens an image file or block device.
    ///
    /// Opens are coordinated per file: a read-write open is exclusive, a
    /// read-only open is shared with other read-only opens. Any other
    /// combination (rw/rw, rw/ro, ro/rw) fails with `EBUSY`, so two
    /// filesystem instances can never write one image. On Unix this is an
    /// advisory `flock` (honoured by other winuxfs processes, not by
    /// arbitrary programs); on Windows the file is opened with a share mode
    /// that denies writers, which is mandatory, and the lock is taken as well.
    /// The lock is released when the device is dropped.
    pub fn open(path: &Path, read_only: bool) -> Result<FileDevice, Errno> {
        let mut oo = OpenOptions::new();
        oo.read(true).write(!read_only);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            // FILE_SHARE_READ only: nobody else may open the file for writing
            // while we hold it, and a writer admits no other opener's write.
            oo.share_mode(1);
        }
        let file = oo.open(path).map_err(|e| io_errno(&e))?;
        let locked = if read_only {
            file.try_lock_shared()
        } else {
            file.try_lock()
        };
        match locked {
            Ok(()) => {}
            Err(std::fs::TryLockError::WouldBlock) => return Err(Errno::EBUSY),
            Err(std::fs::TryLockError::Error(_)) => return Err(Errno::EIO),
        }
        let size = file.metadata().map_err(|e| io_errno(&e))?.len();
        Ok(FileDevice {
            file,
            size,
            read_only,
        })
    }
}

fn io_errno(e: &std::io::Error) -> Errno {
    use std::io::ErrorKind::*;
    // ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION: another open denies us.
    if cfg!(windows) && matches!(e.raw_os_error(), Some(32 | 33)) {
        return Errno::EBUSY;
    }
    match e.kind() {
        NotFound => Errno::ENOENT,
        // Callers tell "needs more privilege" apart from I/O failures.
        PermissionDenied => Errno::EACCES,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_errors_map_to_errno() {
        let missing = std::env::temp_dir().join(format!("winuxfs-nofile-{}", std::process::id()));
        assert_eq!(FileDevice::open(&missing, true).err(), Some(Errno::ENOENT));
        let denied = std::io::Error::from(std::io::ErrorKind::PermissionDenied);
        assert_eq!(io_errno(&denied), Errno::EACCES);
        if cfg!(windows) {
            let sharing = std::io::Error::from_raw_os_error(32);
            assert_eq!(io_errno(&sharing), Errno::EBUSY);
        }
    }
}
