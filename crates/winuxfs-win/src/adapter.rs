// SPDX-License-Identifier: GPL-2.0-only
//! WinFsp callbacks over a path-level [`Vfs`] (read path).
//!
//! # Names
//! WinFsp hands over UTF-16 paths with `\` separators. Each component is
//! mapped with [`namemap::from_windows`] (strict: names that the reverse
//! mapping could not have produced are rejected) and the components are
//! joined with `/` for the Vfs. Directory listings go the other way with
//! [`namemap::to_windows`]. The Vfs never returns `.` or `..`, and none are
//! synthesised.
//!
//! # File contexts
//! The volume is created with `UmFileContextIsUserContext2`, so WinFsp keeps
//! one file context per open handle and calls `Close` exactly once for each
//! successful `Open`. The context *is* the Vfs handle number (never 0). The
//! adapter also records the open handles so that it can close whatever is
//! still open when the file system is torn down.
//!
//! # Directory markers
//! The volume uses `DirectoryMarkerAsNextOffset`: every entry carries the
//! Vfs `read_dir` cookie in its `NextOffset`, and WinFsp hands the cookie of
//! the last entry it consumed back as the marker (8 bytes), so listing
//! resumes exactly where it stopped, even in directories with thousands of
//! entries or ones that change while being read.
//!
//! # Safety net
//! Every callback body runs under `catch_unwind`; a panic becomes
//! `STATUS_UNSUCCESSFUL` and a message on stderr and never unwinds into
//! WinFsp.

// Off Windows only the pure helpers (and their tests) are built.
#![cfg_attr(not(windows), allow(dead_code))]

use std::panic::{catch_unwind, AssertUnwindSafe};

use winuxfs_core::namemap;
use winuxfs_core::plugin::{FileType, Stat, Timespec};
use winuxfs_core::Errno;

use crate::winfsp_ffi::{FSP_FSCTL_FILE_INFO, NTSTATUS};

pub const STATUS_SUCCESS: NTSTATUS = 0;
pub const STATUS_BUFFER_OVERFLOW: NTSTATUS = 0x8000_0005_u32 as i32;
pub const STATUS_UNSUCCESSFUL: NTSTATUS = 0xC000_0001_u32 as i32;
pub const STATUS_INVALID_PARAMETER: NTSTATUS = 0xC000_000D_u32 as i32;
pub const STATUS_END_OF_FILE: NTSTATUS = 0xC000_0011_u32 as i32;
pub const STATUS_ACCESS_DENIED: NTSTATUS = 0xC000_0022_u32 as i32;
pub const STATUS_OBJECT_NAME_INVALID: NTSTATUS = 0xC000_0033_u32 as i32;
pub const STATUS_OBJECT_NAME_NOT_FOUND: NTSTATUS = 0xC000_0034_u32 as i32;
pub const STATUS_OBJECT_NAME_COLLISION: NTSTATUS = 0xC000_0035_u32 as i32;
pub const STATUS_OBJECT_PATH_NOT_FOUND: NTSTATUS = 0xC000_003A_u32 as i32;
pub const STATUS_SHARING_VIOLATION: NTSTATUS = 0xC000_0043_u32 as i32;
pub const STATUS_DISK_FULL: NTSTATUS = 0xC000_007F_u32 as i32;
pub const STATUS_MEDIA_WRITE_PROTECTED: NTSTATUS = 0xC000_00A2_u32 as i32;
pub const STATUS_FILE_IS_A_DIRECTORY: NTSTATUS = 0xC000_00BA_u32 as i32;
pub const STATUS_UNEXPECTED_IO_ERROR: NTSTATUS = 0xC000_00E9_u32 as i32;
pub const STATUS_DIRECTORY_NOT_EMPTY: NTSTATUS = 0xC000_0101_u32 as i32;
pub const STATUS_NOT_A_DIRECTORY: NTSTATUS = 0xC000_0103_u32 as i32;

pub const FILE_ATTRIBUTE_READONLY: u32 = 0x1;
pub const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x10;
pub const FILE_ATTRIBUTE_NORMAL: u32 = 0x80;

/// Allocation granularity reported to Windows (also the volume's sector
/// size).
const ALLOC_UNIT: u64 = 4096;
/// Seconds from 1601-01-01 (FILETIME epoch) to 1970-01-01.
const FILETIME_UNIX_OFFSET: i128 = 11_644_473_600;

/// Maps a Linux errno to the NTSTATUS a Windows caller expects.
///
/// `ENOENT` maps to `STATUS_OBJECT_NAME_NOT_FOUND`; where the parent is
/// missing the callers report `STATUS_OBJECT_PATH_NOT_FOUND` themselves.
/// Unlisted values become `STATUS_UNEXPECTED_IO_ERROR`.
pub fn errno_to_ntstatus(e: Errno) -> NTSTATUS {
    match e {
        Errno::ENOENT => STATUS_OBJECT_NAME_NOT_FOUND,
        Errno::EEXIST => STATUS_OBJECT_NAME_COLLISION,
        Errno::ENOTDIR => STATUS_NOT_A_DIRECTORY,
        Errno::EISDIR => STATUS_FILE_IS_A_DIRECTORY,
        Errno::ENOTEMPTY => STATUS_DIRECTORY_NOT_EMPTY,
        Errno::EACCES => STATUS_ACCESS_DENIED,
        Errno::EROFS => STATUS_MEDIA_WRITE_PROTECTED,
        Errno::ENOSPC => STATUS_DISK_FULL,
        Errno::ENAMETOOLONG => STATUS_OBJECT_NAME_INVALID,
        Errno::EBUSY => STATUS_SHARING_VIOLATION,
        Errno::EINVAL => STATUS_INVALID_PARAMETER,
        _ => STATUS_UNEXPECTED_IO_ERROR,
    }
}

/// Converts a WinFsp path (`\`-separated UTF-16, starting with `\`) to a
/// Vfs path. A single trailing `\` is ignored. Empty components, `.`/`..`
/// and names [`namemap::from_windows`] rejects are `EINVAL`.
pub(crate) fn vfs_path(win: &[u16]) -> Result<Vec<u8>, Errno> {
    const SEP: u16 = b'\\' as u16;
    let rest = win.strip_prefix(&[SEP]).ok_or(Errno::EINVAL)?;
    let rest = rest.strip_suffix(&[SEP]).unwrap_or(rest);
    let mut out = Vec::with_capacity(win.len() + 1);
    if rest.is_empty() {
        out.push(b'/');
        return Ok(out);
    }
    for comp in rest.split(|&u| u == SEP) {
        let name = namemap::from_windows(comp)?;
        if name.is_empty() || name == b"." || name == b".." {
            return Err(Errno::EINVAL);
        }
        out.push(b'/');
        out.extend_from_slice(&name);
    }
    Ok(out)
}

/// Unix time to Windows FILETIME (100 ns ticks since 1601-01-01), clamped
/// to `0..=i64::MAX` (Windows treats FILETIME as signed); `nsec` is clamped
/// to the last tick of its second.
pub(crate) fn filetime(t: Timespec) -> u64 {
    let nsec = i128::from(t.nsec.min(999_999_999));
    let ticks = (i128::from(t.sec) + FILETIME_UNIX_OFFSET) * 10_000_000 + nsec / 100;
    ticks.clamp(0, i128::from(i64::MAX)) as u64
}

/// WinFsp file information for `st`.
///
/// Attributes: directories are `DIRECTORY`; a regular file with no write
/// permission bit is `READONLY` unless the whole mount is read-only (the
/// volume flag already says so); everything else is `NORMAL`. The creation
/// time is the birth time when the backend knows it, else the change time.
pub(crate) fn file_info(st: &Stat, read_only_mount: bool) -> FSP_FSCTL_FILE_INFO {
    let file_attributes = match st.ftype {
        FileType::Dir => FILE_ATTRIBUTE_DIRECTORY,
        FileType::Reg if st.mode & 0o222 == 0 && !read_only_mount => FILE_ATTRIBUTE_READONLY,
        _ => FILE_ATTRIBUTE_NORMAL,
    };
    let birth = if st.btime == Timespec::default() {
        st.ctime
    } else {
        st.btime
    };
    FSP_FSCTL_FILE_INFO {
        file_attributes,
        reparse_tag: 0,
        allocation_size: st.size.div_ceil(ALLOC_UNIT).saturating_mul(ALLOC_UNIT),
        file_size: st.size,
        creation_time: filetime(birth),
        last_access_time: filetime(st.atime),
        last_write_time: filetime(st.mtime),
        change_time: filetime(st.ctime),
        index_number: st.ino,
        hard_links: 0,
        ea_size: 0,
    }
}

/// Runs a callback body, turning a panic into `STATUS_UNSUCCESSFUL`.
pub(crate) fn guard(name: &str, f: impl FnOnce() -> NTSTATUS) -> NTSTATUS {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(st) => st,
        Err(p) => {
            log_panic(name, &*p);
            STATUS_UNSUCCESSFUL
        }
    }
}

/// Same as [`guard`] for callbacks without a result.
pub(crate) fn guard_void(name: &str, f: impl FnOnce()) {
    if let Err(p) = catch_unwind(AssertUnwindSafe(f)) {
        log_panic(name, &*p);
    }
}

fn log_panic(name: &str, p: &(dyn std::any::Any + Send)) {
    let msg = p
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| p.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("(non-string panic payload)");
    // Best effort: stderr may be closed; never panic here.
    use std::io::Write;
    let _ = writeln!(
        std::io::stderr(),
        "winuxfs: panic in WinFsp callback {name}: {msg}"
    );
}

/// Options for [`mount_vfs`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MountOptions {
    /// Volume label (at most 32 UTF-16 units are shown; longer is cut).
    pub volume_label: String,
    /// Sets WinFsp's `ReadOnlyVolume`: Windows refuses every modification.
    pub read_only: bool,
    /// Must match the Vfs: `true` iff the Vfs is not case-insensitive.
    pub case_sensitive: bool,
}

#[cfg(windows)]
pub use mount::{mount_vfs, MountedFs};

#[cfg(windows)]
mod mount;

#[cfg(test)]
mod tests {
    use super::*;
    use winuxfs_core::plugin::{FileType, Stat, Timespec};
    use winuxfs_core::Errno;

    #[test]
    fn panics_in_callbacks_become_status_unsuccessful() {
        assert_eq!(guard("t", || STATUS_END_OF_FILE), STATUS_END_OF_FILE);
        assert_eq!(guard("t", || panic!("boom")), STATUS_UNSUCCESSFUL);
        assert_eq!(
            guard("t", || std::panic::panic_any(7u32)),
            STATUS_UNSUCCESSFUL
        );
        let mut ran = false;
        guard_void("t", || ran = true);
        assert!(ran);
        guard_void("t", || panic!("boom"));
    }

    #[test]
    fn errno_table_is_total() {
        let table: &[(i32, u32)] = &[
            (2, 0xC000_0034),  // ENOENT
            (17, 0xC000_0035), // EEXIST
            (20, 0xC000_0103), // ENOTDIR
            (21, 0xC000_00BA), // EISDIR
            (39, 0xC000_0101), // ENOTEMPTY
            (13, 0xC000_0022), // EACCES
            (30, 0xC000_00A2), // EROFS
            (28, 0xC000_007F), // ENOSPC
            (36, 0xC000_0033), // ENAMETOOLONG
            (16, 0xC000_0043), // EBUSY
            (22, 0xC000_000D), // EINVAL
        ];
        for &(e, st) in table {
            assert_eq!(errno_to_ntstatus(Errno(e)) as u32, st, "errno {e}");
        }
        for e in [0, 1, 5, 9, 12, 38, 75, 133, 134, -1, i32::MAX, i32::MIN] {
            if table.iter().any(|&(t, _)| t == e) {
                continue;
            }
            assert_eq!(
                errno_to_ntstatus(Errno(e)),
                STATUS_UNEXPECTED_IO_ERROR,
                "errno {e}"
            );
        }
        assert_eq!(STATUS_UNEXPECTED_IO_ERROR as u32, 0xC000_00E9);
    }

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn windows_paths_map_per_component() {
        assert_eq!(vfs_path(&w("\\")).unwrap(), b"/");
        assert_eq!(
            vfs_path(&w("\\dir1\\hello.txt")).unwrap(),
            b"/dir1/hello.txt"
        );
        // Reserved characters come back as their disk bytes.
        assert_eq!(vfs_path(&w("\\a\u{F03A}b")).unwrap(), b"/a:b");
        assert_eq!(vfs_path(&w("\\d\\x\u{F02E}")).unwrap(), b"/d/x.");
        // Invalid disk bytes round-trip.
        let mut p = w("\\");
        p.extend(winuxfs_core::namemap::to_windows(&[0x66, 0xFF]));
        assert_eq!(vfs_path(&p).unwrap(), [b'/', 0x66, 0xFF]);
        // Trailing separator (a directory opened as "\d\") is tolerated.
        assert_eq!(vfs_path(&w("\\d\\")).unwrap(), b"/d");
        for bad in ["", "rel", "\\a\\\\b", "\\a:b", "\\.", "\\a\\..", "\\x."] {
            assert_eq!(vfs_path(&w(bad)).unwrap_err(), Errno::EINVAL, "{bad:?}");
        }
        // Lone surrogate.
        assert_eq!(vfs_path(&[0x5C, 0xD800]).unwrap_err(), Errno::EINVAL);
    }

    #[test]
    fn filetime_conversion_and_clamping() {
        let t = |sec, nsec| Timespec { sec, nsec };
        assert_eq!(filetime(t(0, 0)), 116_444_736_000_000_000);
        assert_eq!(filetime(t(1, 999)), 116_444_736_000_000_000 + 10_000_009);
        assert_eq!(filetime(t(-11_644_473_600, 0)), 0);
        assert_eq!(filetime(t(-11_644_473_601, 0)), 0);
        assert_eq!(filetime(t(i64::MIN, 0)), 0);
        assert_eq!(filetime(t(i64::MAX, 999_999_999)), i64::MAX as u64);
        // nsec beyond a second is clamped, not carried into a wrong value.
        assert_eq!(
            filetime(t(0, u32::MAX)),
            116_444_736_000_000_000 + 9_999_999
        );
    }

    fn stat(ftype: FileType, mode: u32, size: u64) -> Stat {
        Stat {
            ino: 42,
            ftype,
            mode,
            uid: 0,
            gid: 0,
            nlink: 1,
            size,
            blocks: 0,
            atime: Timespec { sec: 10, nsec: 0 },
            mtime: Timespec { sec: 20, nsec: 0 },
            ctime: Timespec { sec: 30, nsec: 0 },
            btime: Timespec::default(),
        }
    }

    #[test]
    fn file_info_attributes_and_sizes() {
        let d = file_info(&stat(FileType::Dir, 0o755, 0), false);
        assert_eq!(d.file_attributes, FILE_ATTRIBUTE_DIRECTORY);
        let f = file_info(&stat(FileType::Reg, 0o644, 5000), false);
        assert_eq!(f.file_attributes, FILE_ATTRIBUTE_NORMAL);
        assert_eq!((f.file_size, f.allocation_size), (5000, 8192));
        assert_eq!(f.index_number, 42);
        assert_eq!(f.last_write_time, filetime(Timespec { sec: 20, nsec: 0 }));
        assert_eq!(f.last_access_time, filetime(Timespec { sec: 10, nsec: 0 }));
        assert_eq!(f.change_time, filetime(Timespec { sec: 30, nsec: 0 }));
        // No birth time: creation time falls back to the change time.
        assert_eq!(f.creation_time, f.change_time);
        let ro = file_info(&stat(FileType::Reg, 0o444, 0), false);
        assert_eq!(ro.file_attributes, FILE_ATTRIBUTE_READONLY);
        // On a read-only mount the volume flag already says so.
        let ro_mount = file_info(&stat(FileType::Reg, 0o444, 0), true);
        assert_eq!(ro_mount.file_attributes, FILE_ATTRIBUTE_NORMAL);
    }
}
