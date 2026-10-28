use std::fmt;

/// Positive Linux errno value.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct Errno(pub i32);

impl Errno {
    pub const ENOENT: Errno = Errno(2);
    pub const EIO: Errno = Errno(5);
    pub const EBADF: Errno = Errno(9);
    pub const EACCES: Errno = Errno(13);
    pub const EBUSY: Errno = Errno(16);
    pub const EEXIST: Errno = Errno(17);
    pub const ENODEV: Errno = Errno(19);
    pub const ENOTDIR: Errno = Errno(20);
    pub const EISDIR: Errno = Errno(21);
    pub const EINVAL: Errno = Errno(22);
    pub const EFBIG: Errno = Errno(27);
    pub const ENOSPC: Errno = Errno(28);
    pub const EROFS: Errno = Errno(30);
    pub const ENAMETOOLONG: Errno = Errno(36);
    pub const ENOSYS: Errno = Errno(38);
    pub const ENOTEMPTY: Errno = Errno(39);
    pub const EOVERFLOW: Errno = Errno(75);

    /// Convert a plugin ABI return value: `0` is success, negative is `-errno`,
    /// positive is invalid and maps to `EIO`.
    pub fn from_abi(r: i32) -> Result<(), Errno> {
        match r {
            0 => Ok(()),
            r if r < 0 => Err(Errno(r.checked_neg().unwrap_or(i32::MAX))),
            _ => Err(Errno::EIO),
        }
    }
}

impl fmt::Debug for Errno {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Errno({})", self.0)
    }
}

impl fmt::Display for Errno {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "errno {}", self.0)
    }
}

impl std::error::Error for Errno {}

impl Errno {
    /// The portable `io::ErrorKind` for this errno (`Other` when there is no
    /// specific kind).
    pub fn io_kind(self) -> std::io::ErrorKind {
        use std::io::ErrorKind as K;
        match self.0 {
            1 | 13 => K::PermissionDenied,
            2 => K::NotFound,
            16 => K::ResourceBusy,
            17 => K::AlreadyExists,
            20 => K::NotADirectory,
            21 => K::IsADirectory,
            22 => K::InvalidInput,
            27 => K::FileTooLarge,
            28 => K::StorageFull,
            30 => K::ReadOnlyFilesystem,
            36 => K::InvalidFilename,
            38 => K::Unsupported,
            39 => K::DirectoryNotEmpty,
            _ => K::Other,
        }
    }
}

/// On Linux the errno is the OS error number, so the raw code is kept (and
/// the kind follows from it). Elsewhere (Windows) the numbers mean other
/// errors, so the error is built from [`Errno::io_kind`] instead.
impl From<Errno> for std::io::Error {
    fn from(e: Errno) -> Self {
        if cfg!(target_os = "linux") {
            std::io::Error::from_raw_os_error(e.0)
        } else {
            std::io::Error::new(e.io_kind(), e)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn from_abi_maps() {
        assert_eq!(Errno::from_abi(0), Ok(()));
        assert_eq!(Errno::from_abi(-2), Err(Errno::ENOENT));
        assert_eq!(Errno::from_abi(7), Err(Errno::EIO));
        assert_eq!(Errno::from_abi(i32::MIN), Err(Errno(i32::MAX)));
    }

    #[test]
    fn converts_to_io_error_by_kind() {
        use std::io::ErrorKind as K;
        let table = [
            (Errno::ENOENT, K::NotFound),
            (Errno::EACCES, K::PermissionDenied),
            (Errno(1), K::PermissionDenied),
            (Errno::EEXIST, K::AlreadyExists),
            (Errno::ENOTDIR, K::NotADirectory),
            (Errno::EISDIR, K::IsADirectory),
            (Errno::EINVAL, K::InvalidInput),
            (Errno::ENOSPC, K::StorageFull),
            (Errno::EROFS, K::ReadOnlyFilesystem),
            (Errno::ENAMETOOLONG, K::InvalidFilename),
            (Errno::ENOTEMPTY, K::DirectoryNotEmpty),
            (Errno::EBUSY, K::ResourceBusy),
            (Errno::EFBIG, K::FileTooLarge),
            (Errno::ENOSYS, K::Unsupported),
        ];
        for (e, k) in table {
            assert_eq!(e.io_kind(), k, "{e:?}");
            let io: std::io::Error = e.into();
            assert_eq!(io.kind(), k, "{e:?}");
            // Linux keeps the raw errno; elsewhere the number would be a
            // different (wrong) OS error, so none is attached.
            if cfg!(target_os = "linux") {
                assert_eq!(io.raw_os_error(), Some(e.0));
            } else {
                assert_eq!(io.raw_os_error(), None);
            }
        }
        assert_eq!(Errno::EIO.io_kind(), K::Other);
        assert_eq!(Errno(-5).io_kind(), K::Other);
    }
}
