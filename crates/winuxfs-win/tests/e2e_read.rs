// SPDX-License-Identifier: GPL-2.0-only
//! End-to-end tests for the WinFsp adapter's read path. They need Windows
//! with WinFsp 2.x (the test VM, docs/vm.md) and the cross-built memfs
//! plugin `winuxfs_backend_memfs.dll`, found next to the test executable or
//! at `WINUXFS_MEMFS_DLL`:
//!
//! ```sh
//! make -C backends/memfs CC=x86_64-w64-mingw32-gcc EXT=dll
//! cargo test -p winuxfs-win --target x86_64-pc-windows-gnu --no-run
//! # push both files into one directory in the VM and run the exe there
//! ```
//!
//! Each test mounts its own memfs instance on [`DRIVE`]; a lock serialises
//! them because they share the drive letter. `Z:` is avoided: the VM maps
//! its Samba share there.
#![cfg(windows)]

use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use winuxfs_core::plugin::{load_plugin, Devices, FileType};
use winuxfs_core::vfs::{Vfs, VfsOptions};
use winuxfs_win::{mount_vfs, MountOptions, MountedFs, WinFsp};

/// Drive letter the tests mount on.
const DRIVE: &str = "Y:";
const LABEL: &str = "winuxfs-test";
const HELLO: &[u8] = b"Hello from winuxfs memfs!\r\n";
const NESTED: &[u8] = b"nested file content";
const BIG_DIR_FILES: usize = 10_000;

static LOCK: Mutex<()> = Mutex::new(());

fn lock() -> MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

fn memfs_dll() -> PathBuf {
    if let Some(p) = std::env::var_os("WINUXFS_MEMFS_DLL") {
        return PathBuf::from(p);
    }
    let exe = std::env::current_exe().unwrap();
    exe.parent().unwrap().join("winuxfs_backend_memfs.dll")
}

fn root(rel: &str) -> PathBuf {
    PathBuf::from(format!("{DRIVE}\\{rel}"))
}

fn mkfile(v: &Vfs, p: &[u8], data: &[u8], mode: u32) {
    let (h, _) = v.create(p, FileType::Reg, mode).unwrap();
    assert_eq!(v.write(h, 0, data).unwrap(), data.len());
    v.close(h);
}

fn mkdir(v: &Vfs, p: &[u8]) {
    let (h, _) = v.create(p, FileType::Dir, 0o755).unwrap();
    v.close(h);
}

struct Setup {
    big_dir: bool,
    read_only: bool,
}

/// A fresh memfs with the fixture tree, wrapped in a Vfs with the
/// requested options.
fn fixture(s: Setup) -> Arc<Vfs> {
    let backend = load_plugin(&memfs_dll()).expect("load memfs plugin");
    let devs = Devices {
        data: None,
        log: None,
        rt: None,
    };
    let mount = Arc::new(backend.mount(devs, "").expect("mount memfs"));
    {
        let w = Vfs::new(mount.clone(), VfsOptions::default());
        mkfile(&w, b"/hello.txt", HELLO, 0o644);
        mkdir(&w, b"/dir1");
        mkfile(&w, b"/dir1/nested.txt", NESTED, 0o644);
        mkdir(&w, b"/dir1/sub");
        mkfile(&w, b"/a:b", b"reserved", 0o644);
        mkfile(&w, b"/readonly.txt", b"ro", 0o444);
        if s.big_dir {
            mkdir(&w, b"/big");
            for i in 0..BIG_DIR_FILES {
                mkfile(&w, format!("/big/file{i:05}").as_bytes(), b"", 0o644);
            }
        }
    }
    Arc::new(Vfs::new(
        mount,
        VfsOptions {
            case_insensitive: false,
            read_only: s.read_only,
        },
    ))
}

struct Mounted {
    fs: Option<MountedFs>,
    vfs: Arc<Vfs>,
    _guard: MutexGuard<'static, ()>,
}

fn mount_with(s: Setup) -> Mounted {
    let guard = lock();
    let read_only = s.read_only;
    let vfs = fixture(s);
    let fsp = WinFsp::load().expect("load WinFsp");
    let opts = MountOptions {
        volume_label: LABEL.to_string(),
        read_only,
        case_sensitive: true,
    };
    let fs = mount_vfs(&fsp, vfs.clone(), DRIVE, &opts).expect("mount_vfs");
    Mounted {
        fs: Some(fs),
        vfs,
        _guard: guard,
    }
}

fn mount() -> Mounted {
    mount_with(Setup {
        big_dir: false,
        read_only: false,
    })
}

fn names(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().into_string().unwrap())
        .collect();
    v.sort();
    v
}

#[test]
fn lists_root() {
    let _m = mount();
    let n = names(&root(""));
    for want in ["hello.txt", "dir1", "readonly.txt"] {
        assert!(n.iter().any(|x| x == want), "{want} missing from {n:?}");
    }
    assert_eq!(n.len(), 4, "{n:?}");
}

#[test]
fn reads_file_content() {
    let _m = mount();
    assert_eq!(std::fs::read(root("hello.txt")).unwrap(), HELLO);
    assert_eq!(std::fs::read(root("dir1\\nested.txt")).unwrap(), NESTED);
    // Reads at an offset and past the end.
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(root("hello.txt")).unwrap();
    f.seek(SeekFrom::Start(6)).unwrap();
    let mut buf = [0u8; 4];
    f.read_exact(&mut buf).unwrap();
    assert_eq!(&buf, &HELLO[6..10]);
    f.seek(SeekFrom::Start(1000)).unwrap();
    assert_eq!(f.read(&mut buf).unwrap(), 0);
}

#[test]
fn nested_dir_listing() {
    let _m = mount();
    assert_eq!(names(&root("dir1")), ["nested.txt", "sub"]);
    assert!(names(&root("dir1\\sub")).is_empty());
}

#[test]
fn stat_sizes_and_dir_flag() {
    let _m = mount();
    let d = std::fs::metadata(root("dir1")).unwrap();
    assert!(d.is_dir());
    let f = std::fs::metadata(root("hello.txt")).unwrap();
    assert!(f.is_file());
    assert_eq!(f.len(), HELLO.len() as u64);
    assert!(!f.permissions().readonly());
    assert!(std::fs::metadata(root("readonly.txt"))
        .unwrap()
        .permissions()
        .readonly());
    // Sizes from the directory listing match too.
    for e in std::fs::read_dir(root("dir1")).unwrap() {
        let e = e.unwrap();
        let md = e.metadata().unwrap();
        match e.file_name().to_str().unwrap() {
            "nested.txt" => assert_eq!(md.len(), NESTED.len() as u64),
            "sub" => assert!(md.is_dir()),
            o => panic!("unexpected {o}"),
        }
    }
    let err = std::fs::metadata(root("nope.txt")).unwrap_err();
    assert_eq!(err.kind(), std::io::ErrorKind::NotFound);
    let err = std::fs::metadata(root("nope\\x.txt")).unwrap_err();
    assert_eq!(err.kind(), std::io::ErrorKind::NotFound);
}

#[test]
fn timestamps_are_sane() {
    let _m = mount();
    let ten_years = Duration::from_secs(10 * 365 * 24 * 3600);
    let now = SystemTime::now();
    for p in ["hello.txt", "dir1"] {
        let md = std::fs::metadata(root(p)).unwrap();
        for t in [
            md.modified().unwrap(),
            md.accessed().unwrap(),
            md.created().unwrap(),
        ] {
            assert!(t > UNIX_EPOCH, "{p}: {t:?}");
            assert!(t < now + ten_years, "{p}: {t:?}");
        }
    }
}

#[test]
fn reserved_char_name_visible() {
    let _m = mount();
    let mapped = "a\u{F03A}b";
    assert!(names(&root("")).iter().any(|n| n == mapped));
    assert_eq!(std::fs::read(root(mapped)).unwrap(), b"reserved");
}

#[test]
fn big_dir_listing_10k() {
    let _m = mount_with(Setup {
        big_dir: true,
        read_only: false,
    });
    let n = names(&root("big"));
    assert_eq!(n.len(), BIG_DIR_FILES);
    let mut dedup = n.clone();
    dedup.dedup();
    assert_eq!(dedup.len(), BIG_DIR_FILES);
    assert_eq!(n[0], "file00000");
    assert_eq!(
        n[BIG_DIR_FILES - 1],
        format!("file{:05}", BIG_DIR_FILES - 1)
    );
}

fn wide(s: &str) -> Vec<u16> {
    std::ffi::OsStr::new(s)
        .encode_wide()
        .chain(Some(0))
        .collect()
}

#[test]
fn volume_label_and_space() {
    use windows_sys::Win32::Storage::FileSystem::{GetDiskFreeSpaceExW, GetVolumeInformationW};
    let _m = mount();
    let rootw = wide(&format!("{DRIVE}\\"));
    let (mut avail, mut total, mut free) = (0u64, 0u64, 0u64);
    // SAFETY: rootw is NUL-terminated; out pointers are valid.
    let ok = unsafe { GetDiskFreeSpaceExW(rootw.as_ptr(), &mut avail, &mut total, &mut free) };
    assert_ne!(ok, 0, "{}", std::io::Error::last_os_error());
    assert_eq!(total, 1 << 30);
    assert!(free > 0 && free <= total);
    let mut label = [0u16; 64];
    let mut fsname = [0u16; 64];
    let (mut serial, mut maxlen, mut flags) = (0u32, 0u32, 0u32);
    // SAFETY: buffers and lengths match; out pointers are valid.
    let ok = unsafe {
        GetVolumeInformationW(
            rootw.as_ptr(),
            label.as_mut_ptr(),
            label.len() as u32,
            &mut serial,
            &mut maxlen,
            &mut flags,
            fsname.as_mut_ptr(),
            fsname.len() as u32,
        )
    };
    assert_ne!(ok, 0, "{}", std::io::Error::last_os_error());
    let end = label.iter().position(|&c| c == 0).unwrap();
    assert_eq!(String::from_utf16(&label[..end]).unwrap(), LABEL);
    assert_eq!(maxlen, 255);
    const FILE_CASE_SENSITIVE_SEARCH: u32 = 1;
    const FILE_READ_ONLY_VOLUME: u32 = 0x80000;
    assert_ne!(flags & FILE_CASE_SENSITIVE_SEARCH, 0);
    assert_eq!(flags & FILE_READ_ONLY_VOLUME, 0);
}

#[test]
fn read_only_mount_rejects_create() {
    let _m = mount_with(Setup {
        big_dir: false,
        read_only: true,
    });
    assert_eq!(std::fs::read(root("hello.txt")).unwrap(), HELLO);
    let err = std::fs::File::create(root("new.txt")).unwrap_err();
    eprintln!("create on read-only mount: {err:?}");
    assert!(
        matches!(
            err.kind(),
            std::io::ErrorKind::PermissionDenied | std::io::ErrorKind::ReadOnlyFilesystem
        ),
        "{err:?}"
    );
    let err = std::fs::OpenOptions::new()
        .write(true)
        .open(root("hello.txt"))
        .unwrap_err();
    assert!(
        matches!(
            err.kind(),
            std::io::ErrorKind::PermissionDenied | std::io::ErrorKind::ReadOnlyFilesystem
        ),
        "{err:?}"
    );
    assert!(!root("new.txt").exists());
    // Other modifications are refused too, and nothing reaches the Vfs.
    assert!(std::fs::remove_file(root("hello.txt")).is_err());
    assert!(std::fs::rename(root("hello.txt"), root("x.txt")).is_err());
    assert!(std::fs::create_dir(root("newdir")).is_err());
    assert!(std::fs::remove_dir(root("dir1\\sub")).is_err());
    // WRITE_DAC / WRITE_OWNER: the owner gets WRITE_DAC implicitly from
    // the descriptor, so this reaches the adapter's own read-only check.
    use std::os::windows::fs::OpenOptionsExt;
    for access in [
        0x4_0000u32, /* WRITE_DAC */
        0x8_0000,    /* WRITE_OWNER */
    ] {
        let r = std::fs::OpenOptions::new()
            .access_mode(access)
            .open(root("hello.txt"));
        assert!(
            r.is_err(),
            "access {access:#x} granted on a read-only mount"
        );
    }
    assert_eq!(std::fs::read(root("hello.txt")).unwrap(), HELLO);
    assert!(root("dir1\\sub").is_dir());
    assert_eq!(names(&root("")).len(), 4);
}

/// The read-only mount reports itself as such: the volume flag, and no
/// per-file READONLY attribute noise.
#[test]
fn read_only_mount_reports_read_only_volume() {
    use windows_sys::Win32::Storage::FileSystem::GetVolumeInformationW;
    const FILE_READ_ONLY_VOLUME: u32 = 0x80000;
    let _m = mount_with(Setup {
        big_dir: false,
        read_only: true,
    });
    let rootw = wide(&format!("{DRIVE}\\"));
    let mut flags = 0u32;
    // SAFETY: rootw is NUL-terminated; unused outputs are null.
    let ok = unsafe {
        GetVolumeInformationW(
            rootw.as_ptr(),
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut flags,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_ne!(ok, 0, "{}", std::io::Error::last_os_error());
    assert_ne!(flags & FILE_READ_ONLY_VOLUME, 0, "flags {flags:#x}");
}

#[test]
fn mount_rejects_read_only_mismatch() {
    let _g = lock();
    let fsp = WinFsp::load().unwrap();
    for (vfs_ro, mount_ro) in [(true, false), (false, true)] {
        let vfs = fixture(Setup {
            big_dir: false,
            read_only: vfs_ro,
        });
        let opts = MountOptions {
            volume_label: LABEL.to_string(),
            read_only: mount_ro,
            case_sensitive: true,
        };
        assert!(
            mount_vfs(&fsp, vfs.clone(), DRIVE, &opts).is_err(),
            "vfs ro {vfs_ro}, mount ro {mount_ro}"
        );
        assert_eq!(Arc::strong_count(&vfs), 1);
    }
}

/// `dir Y:\` from cmd.exe in this process's logon session (a drive letter
/// defined by a non-service process is visible only there).
#[test]
fn dir_command_lists_root() {
    let _m = mount();
    use std::os::windows::process::CommandExt;
    // raw_arg: cmd.exe does not understand the quoting `arg` would add.
    let out = std::process::Command::new("cmd.exe")
        .raw_arg(format!("/c dir {DRIVE}\\"))
        .output()
        .unwrap();
    let text = String::from_utf8_lossy(&out.stdout);
    println!("--- dir {DRIVE}\\ ---\n{text}");
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(text.contains("hello.txt"), "{text}");
    assert!(text.contains("dir1"), "{text}");
}

#[test]
fn unmount_removes_drive_and_releases_vfs() {
    let mut m = mount();
    assert!(root("hello.txt").exists());
    drop(m.fs.take());
    assert!(!root("hello.txt").exists());
    assert!(std::fs::metadata(format!("{DRIVE}\\")).is_err());
    assert_eq!(
        Arc::strong_count(&m.vfs),
        1,
        "the adapter still holds the Vfs"
    );
}

#[test]
fn unmount_with_open_handles_does_not_hang() {
    let mut m = mount();
    let mut f = std::fs::File::open(root("hello.txt")).unwrap();
    use std::os::windows::fs::OpenOptionsExt;
    const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
    let d = std::fs::OpenOptions::new()
        .read(true)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
        .open(root("dir1"))
        .expect("open a directory handle");
    let fs = m.fs.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        drop(fs);
        tx.send(()).unwrap();
    });
    rx.recv_timeout(Duration::from_secs(30))
        .expect("MountedFs drop hung with open handles");
    use std::io::Read;
    let mut buf = [0u8; 8];
    assert!(
        f.read(&mut buf).is_err(),
        "read through a dead mount succeeded"
    );
    drop(d);
    drop(f);
    assert_eq!(Arc::strong_count(&m.vfs), 1);
}

#[test]
fn mount_rejects_case_mode_mismatch() {
    let _g = lock();
    let vfs = fixture(Setup {
        big_dir: false,
        read_only: false,
    });
    let fsp = WinFsp::load().unwrap();
    let opts = MountOptions {
        volume_label: LABEL.to_string(),
        read_only: false,
        case_sensitive: false,
    };
    assert!(mount_vfs(&fsp, vfs.clone(), DRIVE, &opts).is_err());
    assert_eq!(Arc::strong_count(&vfs), 1);
}

/// Review Focus 1: a 10,000-entry directory read through a small buffer
/// (many directory queries), with 1,000 already-listed files (including the
/// resume marker entry) and 500 not yet listed files deleted and 50 files
/// created through the Vfs midway, lists every remaining file exactly once.
#[test]
fn big_dir_small_buffer_paging_with_deletes() {
    use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, FileFullDirectoryInfo, GetFileInformationByHandleEx,
        FILE_FLAG_BACKUP_SEMANTICS, FILE_FULL_DIR_INFO, FILE_LIST_DIRECTORY, FILE_SHARE_DELETE,
        FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
    };
    const ERROR_NO_MORE_FILES: u32 = 18;
    let m = mount_with(Setup {
        big_dir: true,
        read_only: false,
    });
    let path = wide(&format!("{DRIVE}\\big"));
    // SAFETY: path is NUL-terminated; the handle is closed below.
    let h = unsafe {
        CreateFileW(
            path.as_ptr(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            std::ptr::null(),
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            std::ptr::null_mut(),
        )
    };
    assert_ne!(
        h,
        INVALID_HANDLE_VALUE,
        "{}",
        std::io::Error::last_os_error()
    );
    let mut seen: Vec<String> = Vec::new();
    let mut deleted = std::collections::HashSet::new();
    let mut unlisted_deleted = std::collections::HashSet::new();
    let mut dots = 0;
    let mut buf = vec![0u64; 64]; // 512 bytes: a handful of entries per query
    let mut queries = 0;
    loop {
        // SAFETY: buf has the given size; h is a directory handle.
        let ok = unsafe {
            GetFileInformationByHandleEx(
                h,
                FileFullDirectoryInfo,
                buf.as_mut_ptr().cast(),
                (buf.len() * 8) as u32,
            )
        };
        if ok == 0 {
            // SAFETY: no preconditions.
            let err = unsafe { GetLastError() };
            assert_eq!(err, ERROR_NO_MORE_FILES, "query failed");
            break;
        }
        queries += 1;
        let base = buf.as_ptr() as *const u8;
        let mut off = 0usize;
        loop {
            // SAFETY: the API filled a chain of FILE_FULL_DIR_INFO records
            // inside buf.
            let (next, name) = unsafe {
                let rec = base.add(off) as *const FILE_FULL_DIR_INFO;
                let len = (*rec).FileNameLength as usize / 2;
                let np = std::ptr::addr_of!((*rec).FileName) as *const u16;
                (
                    (*rec).NextEntryOffset as usize,
                    String::from_utf16(std::slice::from_raw_parts(np, len)).unwrap(),
                )
            };
            if name == "." || name == ".." {
                dots += 1;
            } else {
                seen.push(name);
            }
            if next == 0 {
                break;
            }
            off += next;
        }
        if deleted.is_empty() && seen.len() >= 2000 {
            // Delete 1,000 files that were already listed, including the
            // last one returned (the entry the next query resumes after),
            // 500 files not listed yet, and create 50 new files.
            let mut victims: Vec<String> = seen.iter().take(999).cloned().collect();
            victims.push(seen.last().unwrap().clone());
            for name in &victims {
                m.vfs.unlink(format!("/big/{name}").as_bytes()).unwrap();
                deleted.insert(name.clone());
            }
            let listed: std::collections::HashSet<&String> = seen.iter().collect();
            for i in (0..BIG_DIR_FILES).rev() {
                if unlisted_deleted.len() == 500 {
                    break;
                }
                let n = format!("file{i:05}");
                if !listed.contains(&n) {
                    m.vfs.unlink(format!("/big/{n}").as_bytes()).unwrap();
                    unlisted_deleted.insert(n);
                }
            }
            for i in 0..50 {
                let (h, _) = m
                    .vfs
                    .create(format!("/big/new{i:02}").as_bytes(), FileType::Reg, 0o644)
                    .unwrap();
                m.vfs.close(h);
            }
        }
    }
    // SAFETY: h came from CreateFileW.
    unsafe { CloseHandle(h) };
    eprintln!("{} entries in {queries} queries", seen.len());
    assert!(
        queries > 100,
        "buffer was not small enough: {queries} queries"
    );
    let unique: std::collections::HashSet<&String> = seen.iter().collect();
    assert_eq!(unique.len(), seen.len(), "duplicate entries");
    assert_eq!(dots, 2, "\".\" and \"..\" once each");
    assert_eq!(deleted.len(), 1000);
    assert_eq!(unlisted_deleted.len(), 500);
    for i in 0..BIG_DIR_FILES {
        let n = format!("file{i:05}");
        if unlisted_deleted.contains(&n) {
            assert!(!unique.contains(&n), "{n} was deleted before it was listed");
        } else {
            assert!(
                unique.contains(&n) || deleted.contains(&n),
                "{n} missing from the listing"
            );
        }
    }
    // Files created mid-listing may or may not show up, but at most once
    // (checked above) and with their real names.
    for n in &seen {
        assert!(n.starts_with("file") || n.starts_with("new"), "{n}");
    }
}

/// A non-root directory lists "." and ".." first (as on NTFS), so the
/// first query of an empty directory succeeds; the root has neither.
#[test]
fn directory_queries_include_dot_entries() {
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, FileIdBothDirectoryRestartInfo, GetFileInformationByHandleEx,
        FILE_FLAG_BACKUP_SEMANTICS, FILE_ID_BOTH_DIR_INFO, FILE_LIST_DIRECTORY, FILE_SHARE_READ,
        FILE_SHARE_WRITE, OPEN_EXISTING,
    };
    let _m = mount();
    let list = |rel: &str| -> Vec<String> {
        let path = wide(&format!("{DRIVE}\\{rel}"));
        // SAFETY: path is NUL-terminated; the handle is closed below.
        let h = unsafe {
            CreateFileW(
                path.as_ptr(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                std::ptr::null(),
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                std::ptr::null_mut(),
            )
        };
        assert_ne!(h, INVALID_HANDLE_VALUE);
        let mut buf = vec![0u64; 1024];
        // SAFETY: buf has the given size; h is a directory handle.
        let ok = unsafe {
            GetFileInformationByHandleEx(
                h,
                FileIdBothDirectoryRestartInfo,
                buf.as_mut_ptr().cast(),
                (buf.len() * 8) as u32,
            )
        };
        let err = std::io::Error::last_os_error();
        let mut out = Vec::new();
        if ok != 0 {
            let base = buf.as_ptr() as *const u8;
            let mut off = 0usize;
            loop {
                // SAFETY: the API filled a chain of records inside buf.
                let (next, name) = unsafe {
                    let rec = base.add(off) as *const FILE_ID_BOTH_DIR_INFO;
                    let len = (*rec).FileNameLength as usize / 2;
                    let np = std::ptr::addr_of!((*rec).FileName) as *const u16;
                    (
                        (*rec).NextEntryOffset as usize,
                        String::from_utf16(std::slice::from_raw_parts(np, len)).unwrap(),
                    )
                };
                out.push(name);
                if next == 0 {
                    break;
                }
                off += next;
            }
        }
        // SAFETY: h came from CreateFileW.
        unsafe { CloseHandle(h) };
        assert_ne!(ok, 0, "first query of {rel:?}: {err}");
        out
    };
    assert_eq!(list("dir1\\sub"), [".", ".."]);
    assert_eq!(list("dir1"), [".", "..", "nested.txt", "sub"]);
    let root_names = list("");
    assert!(
        !root_names.iter().any(|n| n == "." || n == ".."),
        "{root_names:?}"
    );
}
