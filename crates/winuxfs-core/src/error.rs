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

impl From<Errno> for std::io::Error {
    fn from(e: Errno) -> Self {
        std::io::Error::from_raw_os_error(e.0)
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
}
