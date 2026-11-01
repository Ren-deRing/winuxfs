// SPDX-License-Identifier: GPL-2.0-only
//! WinFsp callbacks over a path-level [`Vfs`].
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
//! `.` and `..` are not Vfs entries, but every non-root directory listing
//! starts with them (cookies [`COOKIE_DOT`]/[`COOKIE_DOTDOT`], reserved
//! above 2^63), as on NTFS: Windows code (for example
//! `std::fs::remove_dir_all`) treats an empty first directory query as an
//! error. They carry the directory's own information.
//!
//! # Case-insensitive volumes
//! `Create` and `Open` return the on-disk spelling of the whole path as the
//! normalized name (`FSP_FSCTL_OPEN_FILE_INFO`). Without it WinFsp would
//! keep the upper-cased name the file was opened with and pass that to
//! `Cleanup` and `Rename`. Case-sensitive volumes skip this, as WinFsp's
//! documentation asks.
//!
//! # Writes, deletes and renames
//! - `Write` honours `WriteToEndOfFile` (start at the current size) and
//!   `ConstrainedIo` (never extend the file); see [`write_range`].
//! - Deletion follows WinFsp: `CanDelete` only checks (read-only volume,
//!   root, non-empty directory); `Cleanup` with the delete flag, which
//!   WinFsp sends for the last handle of a delete-pending file, unlinks or
//!   removes the directory. So a file deleted while open keeps its name
//!   until the last handle closes (Windows semantics); the Vfs keeps the
//!   inode readable through handles that are still open.
//! - `Rename` passes `ReplaceIfExists` to the Vfs (without it an existing
//!   target is `STATUS_OBJECT_NAME_COLLISION`); a directory is never
//!   replaced (`STATUS_ACCESS_DENIED`), except by itself (a case-only
//!   rename). Open handles follow the renamed inode.
//! - `SetFileSize` grows with zeros or truncates; allocation size is not
//!   stored (shrinking it below the file size truncates, growing it is a
//!   no-op). `Overwrite` truncates to 0.
//! - Attributes: only `READONLY` on regular files is stored, as "no write
//!   permission bits" ([`create_mode`], [`mode_for_attributes`]); other
//!   attribute bits are accepted and ignored. `SetBasicInfo` sets access
//!   and modification times; creation and change times are accepted and
//!   ignored (the Vfs cannot set them).
//! - `Flush` maps to `Vfs::fsync` (one file) or `Vfs::sync` (volume).
//! - `SetSecurity` is not implemented (WinFsp refuses it), so the fixed
//!   descriptor cannot be changed; on read-only mounts `Open` also refuses
//!   `WRITE_DAC`/`WRITE_OWNER`, which the owner is granted implicitly.
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
pub const STATUS_FILE_CORRUPT_ERROR: NTSTATUS = 0xC000_0102_u32 as i32;
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
        Errno::EUCLEAN => STATUS_FILE_CORRUPT_ERROR,
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

/// The Windows form (`\`-separated UTF-16, no terminator) of a Vfs path,
/// each component mapped with [`namemap::to_windows`]; the inverse of
/// [`vfs_path`]. Used as the normalized name WinFsp stores for an open file.
pub(crate) fn windows_path(path: &[u8]) -> Vec<u16> {
    const SEP: u16 = b'\\' as u16;
    let rest = path.strip_prefix(b"/").unwrap_or(path);
    let mut out = Vec::with_capacity(path.len() + 1);
    if rest.is_empty() {
        out.push(SEP);
        return out;
    }
    for comp in rest.split(|&b| b == b'/') {
        out.push(SEP);
        out.extend(namemap::to_windows(comp));
    }
    out
}

/// The byte range a WinFsp `Write` touches in a file of `size` bytes:
/// `WriteToEndOfFile` starts at `size`; `ConstrainedIo` (paging I/O) never
/// extends the file, so it stops at `size` (length 0 at or past the end).
pub(crate) fn write_range(
    offset: u64,
    len: u32,
    size: u64,
    to_end: bool,
    constrained: bool,
) -> (u64, usize) {
    let offset = if to_end { size } else { offset };
    let len = u64::from(len);
    let len = if constrained {
        len.min(size.saturating_sub(offset))
    } else {
        len
    };
    (offset, len as usize)
}

/// Permission bits for a new file or directory. The only Windows attribute
/// stored is `READONLY` on files (no write bits); on directories Windows
/// treats it as a shell hint, so it is ignored.
pub(crate) fn create_mode(is_dir: bool, attributes: u32) -> u32 {
    if is_dir {
        0o755
    } else if attributes & FILE_ATTRIBUTE_READONLY != 0 {
        0o444
    } else {
        0o644
    }
}

/// New permission bits for a regular file whose attributes are set to
/// `attributes` (only `READONLY` is stored: set clears every write bit,
/// clear gives the owner write access back), or `None` if nothing changes.
pub(crate) fn mode_for_attributes(mode: u32, ftype: FileType, attributes: u32) -> Option<u32> {
    if ftype != FileType::Reg {
        return None;
    }
    let mode = mode & 0o7777;
    let new = if attributes & FILE_ATTRIBUTE_READONLY != 0 {
        mode & !0o222
    } else if mode & 0o222 == 0 {
        mode | 0o200
    } else {
        mode
    };
    (new != mode).then_some(new)
}

/// Unix time to Windows FILETIME (100 ns ticks since 1601-01-01), clamped
/// to `0..=i64::MAX` (Windows treats FILETIME as signed); `nsec` is clamped
/// to the last tick of its second.
pub(crate) fn filetime(t: Timespec) -> u64 {
    let nsec = i128::from(t.nsec.min(999_999_999));
    let ticks = (i128::from(t.sec) + FILETIME_UNIX_OFFSET) * 10_000_000 + nsec / 100;
    ticks.clamp(0, i128::from(i64::MAX)) as u64
}

/// Directory-listing cookie of the synthetic `.` entry. Backend cookies
/// are below 2^63 (like Linux directory offsets; see docs/abi.md), so the
/// two synthetic values cannot collide with them.
pub(crate) const COOKIE_DOT: u64 = u64::MAX - 1;
/// Directory-listing cookie of the synthetic `..` entry.
pub(crate) const COOKIE_DOTDOT: u64 = u64::MAX;
const _: () = assert!(COOKIE_DOT >= 1 << 63);

/// Where a `ReadDirectory` call resumes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DirStart {
    /// At the `.` entry (a fresh listing of a non-root directory).
    Dot,
    /// At the `..` entry.
    DotDot,
    /// At the Vfs entries after this cookie (0 = the first).
    Entries(u64),
}

/// The resume point for `marker` (the NextOffset of the last entry WinFsp
/// consumed, or `None` for a fresh listing). Non-root directories list `.`
/// and `..` first, as on NTFS: Windows code such as `std::fs::remove_dir_all`
/// treats a first query that returns nothing as an error.
pub(crate) fn dir_start(marker: Option<u64>, is_root: bool) -> DirStart {
    match marker {
        None if is_root => DirStart::Entries(0),
        None => DirStart::Dot,
        Some(COOKIE_DOT) => DirStart::DotDot,
        Some(COOKIE_DOTDOT) => DirStart::Entries(0),
        Some(c) => DirStart::Entries(c),
    }
}

/// Windows FILETIME to Unix time, or `None` for the values `SetBasicInfo`
/// uses to mean "leave unchanged": 0, and -1/-2 (as unsigned).
pub(crate) fn timespec_from_filetime(ft: u64) -> Option<Timespec> {
    if ft == 0 || ft >= u64::MAX - 1 || ft > i64::MAX as u64 {
        return None;
    }
    let ticks = ft as i64;
    let sec = ticks.div_euclid(10_000_000) - FILETIME_UNIX_OFFSET as i64;
    let nsec = (ticks.rem_euclid(10_000_000) * 100) as u32;
    Some(Timespec { sec, nsec })
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
    TRACE_PATH.with(|p| p.borrow_mut().clear());
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(st) => {
            if st != 0 {
                trace_failure(name, st);
            }
            st
        }
        Err(p) => {
            log_panic(name, &*p);
            STATUS_UNSUCCESSFUL
        }
    }
}

thread_local! {
    static TRACE_PATH: std::cell::RefCell<String> = const { std::cell::RefCell::new(String::new()) };
}

/// Remembers the path of the callback running on this thread (trace only).
pub(crate) fn trace_path(name: &[u16]) {
    if trace_file().is_some() {
        TRACE_PATH.with(|p| *p.borrow_mut() = String::from_utf16_lossy(name));
    }
}

/// Opt-in diagnostics: `WINUXFS_TRACE=<file>` appends one line per failing callback.
fn trace_file() -> Option<&'static std::sync::Mutex<std::fs::File>> {
    static F: std::sync::OnceLock<Option<std::sync::Mutex<std::fs::File>>> =
        std::sync::OnceLock::new();
    F.get_or_init(|| {
        let path = std::env::var_os("WINUXFS_TRACE")?;
        let f = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .ok()?;
        Some(std::sync::Mutex::new(f))
    })
    .as_ref()
}

/// `WINUXFS_TRACE_ALL=1` additionally logs every successful Open and Read.
pub(crate) fn trace_line(line: impl FnOnce() -> String) {
    use std::io::Write;
    static ALL: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if !*ALL.get_or_init(|| std::env::var_os("WINUXFS_TRACE_ALL").is_some()) {
        return;
    }
    if let Some(f) = trace_file() {
        if let Ok(mut f) = f.lock() {
            let _ = writeln!(f, "{}", line());
        }
    }
}

fn trace_failure(name: &str, st: NTSTATUS) {
    use std::io::Write;
    if let Some(f) = trace_file() {
        let path = TRACE_PATH.with(|p| p.borrow().clone());
        if let Ok(mut f) = f.lock() {
            let _ = writeln!(f, "{name} {:#010x} {path}", st as u32);
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
    /// Also grants access to Authenticated Users. The default descriptor
    /// admits only the mounting user, SYSTEM and Administrators, which locks
    /// out everybody else when the mounting user is SYSTEM (the service).
    pub allow_users: bool,
}

/// The security descriptor (SDDL) of every file of a mount: owner and group
/// are the mounting user `sid`, who gets full access, as do SYSTEM and
/// Administrators (and Authenticated Users with `allow_users`). On a
/// read-only mount they get read and execute access only, so Windows' own
/// access check refuses write opens (WinFsp's `ReadOnlyVolume` flag only
/// reports the volume as read-only; it does not refuse write opens).
pub fn volume_sddl(sid: &str, read_only: bool, allow_users: bool) -> String {
    let r = if read_only { "FRFX" } else { "FA" };
    let users = if allow_users {
        format!("(A;;{r};;;AU)")
    } else {
        String::new()
    };
    format!("O:{sid}G:{sid}D:P(A;;{r};;;{sid})(A;;{r};;;SY)(A;;{r};;;BA){users}")
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
    fn sddl_is_private_unless_users_are_allowed() {
        let a = volume_sddl("S-1-5-18", false, false);
        assert_eq!(
            a,
            "O:S-1-5-18G:S-1-5-18D:P(A;;FA;;;S-1-5-18)(A;;FA;;;SY)(A;;FA;;;BA)"
        );
        assert!(!a.contains("AU"));
        let b = volume_sddl("S-1-5-21-1-2-3-1001", false, true);
        assert!(b.ends_with("(A;;FA;;;BA)(A;;FA;;;AU)"), "{b}");
        let c = volume_sddl("S-1-5-18", true, true);
        assert!(c.ends_with("(A;;FRFX;;;AU)") && !c.contains("FA"), "{c}");
    }

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
            (2, 0xC000_0034),   // ENOENT
            (17, 0xC000_0035),  // EEXIST
            (20, 0xC000_0103),  // ENOTDIR
            (21, 0xC000_00BA),  // EISDIR
            (39, 0xC000_0101),  // ENOTEMPTY
            (13, 0xC000_0022),  // EACCES
            (30, 0xC000_00A2),  // EROFS
            (28, 0xC000_007F),  // ENOSPC
            (36, 0xC000_0033),  // ENAMETOOLONG
            (16, 0xC000_0043),  // EBUSY
            (22, 0xC000_000D),  // EINVAL
            (117, 0xC000_0102), // EUCLEAN -> STATUS_FILE_CORRUPT_ERROR
        ];
        for &(e, st) in table {
            assert_eq!(errno_to_ntstatus(Errno(e)) as u32, st, "errno {e}");
        }
        for e in [
            0,
            1,
            5,
            9,
            12,
            38,
            75,
            116,
            133,
            134,
            -1,
            i32::MAX,
            i32::MIN,
        ] {
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

    #[test]
    fn write_ranges_follow_the_flags() {
        // (offset, len, size, to_end, constrained) -> (offset, len)
        assert_eq!(write_range(0, 10, 0, false, false), (0, 10));
        assert_eq!(write_range(100, 10, 5, false, false), (100, 10));
        // WriteToEndOfFile: the offset is the current size.
        assert_eq!(write_range(0, 10, 5, true, false), (5, 10));
        // ConstrainedIo never extends the file.
        assert_eq!(write_range(0, 10, 5, false, true), (0, 5));
        assert_eq!(write_range(3, 10, 5, false, true), (3, 2));
        assert_eq!(write_range(5, 10, 5, false, true), (5, 0));
        assert_eq!(write_range(9, 10, 5, false, true), (9, 0));
        assert_eq!(write_range(0, 10, 5, true, true), (5, 0));
        // No overflow at the top of the range.
        assert_eq!(
            write_range(u64::MAX - 1, 10, u64::MAX, false, true),
            (u64::MAX - 1, 1)
        );
    }

    #[test]
    fn normalized_names_are_windows_paths() {
        assert_eq!(windows_path(b"/"), w("\\"));
        assert_eq!(windows_path(b"/Dir/Hello.TXT"), w("\\Dir\\Hello.TXT"));
        assert_eq!(windows_path(b"/a:b/x."), w("\\a\u{F03A}b\\x\u{F02E}"));
        // Round trip through vfs_path.
        let p = windows_path(&[b'/', 0x66, 0xFF, b'/', b'k']);
        assert_eq!(vfs_path(&p).unwrap(), [b'/', 0x66, 0xFF, b'/', b'k']);
    }

    #[test]
    fn readonly_attribute_maps_to_write_bits() {
        // New files: READONLY drops every write bit.
        assert_eq!(create_mode(false, 0), 0o644);
        assert_eq!(create_mode(false, FILE_ATTRIBUTE_READONLY), 0o444);
        assert_eq!(create_mode(true, 0), 0o755);
        // Directories ignore READONLY (Windows treats it as a shell hint).
        assert_eq!(create_mode(true, FILE_ATTRIBUTE_READONLY), 0o755);
        // Existing files: set or clear; None = leave unchanged.
        assert_eq!(
            mode_for_attributes(0o644, FileType::Reg, FILE_ATTRIBUTE_READONLY),
            Some(0o444)
        );
        assert_eq!(
            mode_for_attributes(0o444, FileType::Reg, FILE_ATTRIBUTE_NORMAL),
            Some(0o644)
        );
        assert_eq!(mode_for_attributes(0o640, FileType::Reg, 0), None);
        assert_eq!(
            mode_for_attributes(0o4755, FileType::Reg, FILE_ATTRIBUTE_READONLY),
            Some(0o4555)
        );
        assert_eq!(
            mode_for_attributes(0o555, FileType::Reg, FILE_ATTRIBUTE_READONLY),
            None
        );
        assert_eq!(
            mode_for_attributes(0o755, FileType::Dir, FILE_ATTRIBUTE_READONLY),
            None
        );
    }

    #[test]
    fn filetime_to_timespec_and_no_change_values() {
        let t = |sec, nsec| Timespec { sec, nsec };
        assert_eq!(timespec_from_filetime(0), None);
        // -1 and -2 are Windows' "stop/resume updating" markers.
        assert_eq!(timespec_from_filetime(u64::MAX), None);
        assert_eq!(timespec_from_filetime(u64::MAX - 1), None);
        assert_eq!(
            timespec_from_filetime(116_444_736_000_000_000),
            Some(t(0, 0))
        );
        assert_eq!(
            timespec_from_filetime(116_444_736_000_000_000 + 10_000_009),
            Some(t(1, 900))
        );
        assert_eq!(timespec_from_filetime(1), Some(t(-11_644_473_600, 100)));
        for ts in [t(1_000_000_000, 123_456_700), t(-5, 0), t(0, 999_999_900)] {
            assert_eq!(timespec_from_filetime(filetime(ts)), Some(ts));
        }
    }

    #[test]
    fn directory_listing_start_points() {
        use DirStart::*;
        // Non-root directories start with "." and "..", like NTFS.
        assert_eq!(dir_start(None, false), Dot);
        assert_eq!(dir_start(Some(COOKIE_DOT), false), DotDot);
        assert_eq!(dir_start(Some(COOKIE_DOTDOT), false), Entries(0));
        assert_eq!(dir_start(Some(42), false), Entries(42));
        // The root has neither.
        assert_eq!(dir_start(None, true), Entries(0));
        assert_eq!(dir_start(Some(7), true), Entries(7));
    }
}
