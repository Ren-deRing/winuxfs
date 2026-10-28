// SPDX-License-Identifier: GPL-2.0-only
//! End-to-end tests for the WinFsp adapter's write path (Windows only; run
//! in the test VM like `e2e_read.rs`). Each test mounts a fresh memfs on
//! `Y:` and checks the result both through `std::fs` and directly through
//! the Vfs.
#![cfg(windows)]

mod common;

use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Read, Seek, SeekFrom, Write};
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::io::AsRawHandle;
use std::sync::Arc;
use std::time::{Duration, SystemTime};

use common::*;
use winuxfs_core::Errno;

const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;

fn fixture(v: &winuxfs_core::vfs::Vfs) {
    mkfile(v, b"/hello.txt", b"hello", 0o644);
    mkdir(v, b"/dir1");
    mkfile(v, b"/dir1/nested.txt", b"nested", 0o644);
}

fn mount() -> Mounted {
    mount_with(Setup::default(), fixture)
}

fn mount_ci() -> Mounted {
    mount_with(
        Setup {
            case_insensitive: true,
            read_only: false,
        },
        fixture,
    )
}

/// The normalized name Windows has for an open handle, without the volume
/// (`\dir\name`).
fn final_name(f: &File) -> String {
    use windows_sys::Win32::Storage::FileSystem::GetFinalPathNameByHandleW;
    const FILE_NAME_NORMALIZED: u32 = 0;
    const VOLUME_NAME_NONE: u32 = 0x4;
    let mut buf = [0u16; 1024];
    // SAFETY: valid handle and buffer.
    let n = unsafe {
        GetFinalPathNameByHandleW(
            f.as_raw_handle(),
            buf.as_mut_ptr(),
            buf.len() as u32,
            FILE_NAME_NORMALIZED | VOLUME_NAME_NONE,
        )
    };
    assert!(
        n > 0 && (n as usize) < buf.len(),
        "{}",
        std::io::Error::last_os_error()
    );
    String::from_utf16(&buf[..n as usize]).unwrap()
}

fn move_file(from: &str, to: &str, flags: u32) -> std::io::Result<()> {
    use windows_sys::Win32::Storage::FileSystem::MoveFileExW;
    let (f, t) = (wide(from), wide(to));
    // SAFETY: both strings are NUL-terminated.
    if unsafe { MoveFileExW(f.as_ptr(), t.as_ptr(), flags) } == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

#[test]
fn create_write_read() {
    let m = mount();
    let p = root("new.txt");
    fs::write(&p, b"written through Windows").unwrap();
    assert_eq!(fs::read(&p).unwrap(), b"written through Windows");
    assert_eq!(fs::metadata(&p).unwrap().len(), 23);
    assert_eq!(
        vfs_read(&m.vfs, b"/new.txt").unwrap(),
        b"written through Windows"
    );
    // Random access writes, including a hole.
    let mut f = OpenOptions::new().read(true).write(true).open(&p).unwrap();
    f.seek(SeekFrom::Start(30)).unwrap();
    f.write_all(b"tail").unwrap();
    f.seek(SeekFrom::Start(0)).unwrap();
    f.write_all(b"W").unwrap();
    f.flush().unwrap();
    f.sync_all().unwrap();
    drop(f);
    let mut want = b"Written through Windows".to_vec();
    want.resize(30, 0);
    want.extend_from_slice(b"tail");
    assert_eq!(fs::read(&p).unwrap(), want);
    assert_eq!(vfs_read(&m.vfs, b"/new.txt").unwrap(), want);
    // A new directory and a file in it.
    fs::create_dir(root("nd")).unwrap();
    fs::write(root("nd\\x"), b"x").unwrap();
    assert_eq!(vfs_read(&m.vfs, b"/nd/x").unwrap(), b"x");
    // create_new on an existing file fails.
    let err = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&p)
        .unwrap_err();
    assert_eq!(err.kind(), ErrorKind::AlreadyExists);
    // A large write in one call (several WinFsp requests).
    let big: Vec<u8> = (0..3_000_000u32).map(|i| (i % 251) as u8).collect();
    fs::write(root("big.bin"), &big).unwrap();
    assert_eq!(fs::read(root("big.bin")).unwrap(), big);
    assert_eq!(vfs_read(&m.vfs, b"/big.bin").unwrap(), big);
}

#[test]
fn overwrite_truncates() {
    let m = mount();
    let p = root("hello.txt");
    let mut f = File::create(&p).unwrap();
    assert_eq!(f.metadata().unwrap().len(), 0);
    f.write_all(b"new").unwrap();
    drop(f);
    assert_eq!(fs::read(&p).unwrap(), b"new");
    // TRUNCATE_EXISTING as well.
    let f = OpenOptions::new()
        .write(true)
        .truncate(true)
        .open(&p)
        .unwrap();
    assert_eq!(f.metadata().unwrap().len(), 0);
    drop(f);
    assert_eq!(vfs_read(&m.vfs, b"/hello.txt").unwrap(), b"");
}

#[test]
fn append_mode() {
    let m = mount();
    let p = root("hello.txt");
    let mut f = OpenOptions::new().append(true).open(&p).unwrap();
    f.write_all(b" world").unwrap();
    f.write_all(b"!").unwrap();
    drop(f);
    assert_eq!(fs::read(&p).unwrap(), b"hello world!");
    assert_eq!(vfs_read(&m.vfs, b"/hello.txt").unwrap(), b"hello world!");
}

#[test]
fn set_len_grow_shrink() {
    let m = mount();
    let p = root("hello.txt");
    let f = OpenOptions::new().write(true).open(&p).unwrap();
    f.set_len(3 * 4096 + 5).unwrap();
    assert_eq!(f.metadata().unwrap().len(), 3 * 4096 + 5);
    drop(f);
    let data = fs::read(&p).unwrap();
    assert_eq!(data.len(), 3 * 4096 + 5);
    assert_eq!(&data[..5], b"hello");
    assert!(data[5..].iter().all(|&b| b == 0));
    let f = OpenOptions::new().write(true).open(&p).unwrap();
    f.set_len(2).unwrap();
    drop(f);
    assert_eq!(fs::read(&p).unwrap(), b"he");
    assert_eq!(vfs_read(&m.vfs, b"/hello.txt").unwrap(), b"he");
    // Grow again: the bytes cut off before come back as zeros.
    let f = OpenOptions::new().write(true).open(&p).unwrap();
    f.set_len(5).unwrap();
    drop(f);
    assert_eq!(fs::read(&p).unwrap(), b"he\0\0\0");
}

#[test]
fn delete_file_and_dir() {
    let m = mount();
    fs::remove_file(root("dir1\\nested.txt")).unwrap();
    assert!(!root("dir1\\nested.txt").exists());
    assert_eq!(
        m.vfs.open(b"/dir1/nested.txt").map(|_| ()),
        Err(Errno::ENOENT)
    );
    fs::remove_dir(root("dir1")).unwrap();
    assert!(!root("dir1").exists());
    assert_eq!(m.vfs.open(b"/dir1").map(|_| ()), Err(Errno::ENOENT));
    // A tree.
    fs::create_dir_all(root("t\\a\\b")).unwrap();
    fs::write(root("t\\a\\b\\f"), b"f").unwrap();
    fs::write(root("t\\g"), b"g").unwrap();
    let err = fs::remove_dir(root("t")).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::DirectoryNotEmpty);
    fs::remove_dir_all(root("t")).unwrap();
    assert!(!root("t").exists());
    assert_eq!(names(&root("")), ["hello.txt"]);
    // Missing names.
    assert_eq!(
        fs::remove_file(root("nope")).unwrap_err().kind(),
        ErrorKind::NotFound
    );
    // remove_file on a directory / remove_dir on a file fail.
    fs::create_dir(root("d")).unwrap();
    assert!(fs::remove_file(root("d")).is_err());
    assert!(fs::remove_dir(root("hello.txt")).is_err());
    assert!(root("d").is_dir() && root("hello.txt").is_file());
}

#[test]
fn delete_while_open() {
    let m = mount();
    let p = root("hello.txt");
    let mut f = File::open(&p).unwrap();
    fs::remove_file(&p).unwrap();
    // Windows semantics: the name stays until the last handle closes.
    assert!(m
        .vfs
        .open(b"/hello.txt")
        .map(|(h, _)| m.vfs.close(h))
        .is_ok());
    let mut buf = Vec::new();
    f.read_to_end(&mut buf).unwrap();
    assert_eq!(buf, b"hello");
    // A second open of a delete-pending file is refused.
    assert!(File::open(&p).is_err());
    drop(f);
    assert!(!p.exists());
    assert_eq!(m.vfs.open(b"/hello.txt").map(|_| ()), Err(Errno::ENOENT));
    assert!(!names(&root("")).contains(&"hello.txt".to_string()));
    // The name can be reused for a new file.
    fs::write(&p, b"again").unwrap();
    assert_eq!(fs::read(&p).unwrap(), b"again");
}

#[test]
fn delete_dir_while_open() {
    let m = mount();
    fs::create_dir(root("e")).unwrap();
    let d = OpenOptions::new()
        .read(true)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
        .open(root("e"))
        .unwrap();
    fs::remove_dir(root("e")).unwrap();
    assert!(m.vfs.open(b"/e").map(|(h, _)| m.vfs.close(h)).is_ok());
    drop(d);
    assert!(!root("e").exists());
    assert_eq!(m.vfs.open(b"/e").map(|_| ()), Err(Errno::ENOENT));
}

#[test]
fn rename_over_existing_fails_without_replace() {
    let m = mount();
    fs::write(root("a"), b"AAA").unwrap();
    fs::write(root("b"), b"B").unwrap();
    let (a, b) = (format!("{DRIVE}\\a"), format!("{DRIVE}\\b"));
    let err = move_file(&a, &b, 0).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::AlreadyExists, "{err:?}");
    assert_eq!(fs::read(root("a")).unwrap(), b"AAA");
    assert_eq!(fs::read(root("b")).unwrap(), b"B");
    const MOVEFILE_REPLACE_EXISTING: u32 = 1;
    move_file(&a, &b, MOVEFILE_REPLACE_EXISTING).unwrap();
    assert!(!root("a").exists());
    assert_eq!(fs::read(root("b")).unwrap(), b"AAA");
    assert_eq!(vfs_read(&m.vfs, b"/b").unwrap(), b"AAA");
    // To a free name, into another directory, and a directory rename.
    move_file(&b, &format!("{DRIVE}\\dir1\\c"), 0).unwrap();
    assert_eq!(vfs_read(&m.vfs, b"/dir1/c").unwrap(), b"AAA");
    fs::rename(root("dir1"), root("dir2")).unwrap();
    assert_eq!(names(&root("dir2")), ["c", "nested.txt"]);
    assert_eq!(fs::read(root("dir2\\nested.txt")).unwrap(), b"nested");
    assert_eq!(m.vfs.open(b"/dir1").map(|_| ()), Err(Errno::ENOENT));
    // A directory cannot replace a file without the flag either.
    fs::create_dir(root("dd")).unwrap();
    assert!(move_file(&format!("{DRIVE}\\dd"), &format!("{DRIVE}\\hello.txt"), 0).is_err());
    // A missing source.
    assert_eq!(
        fs::rename(root("nope"), root("x")).unwrap_err().kind(),
        ErrorKind::NotFound
    );
}

#[test]
fn rename_open_file() {
    let m = mount();
    let mut f = OpenOptions::new()
        .read(true)
        .write(true)
        .open(root("hello.txt"))
        .unwrap();
    fs::rename(root("hello.txt"), root("dir1\\moved.txt")).unwrap();
    assert!(!root("hello.txt").exists());
    f.seek(SeekFrom::End(0)).unwrap();
    f.write_all(b" after rename").unwrap();
    f.flush().unwrap();
    drop(f);
    assert_eq!(
        fs::read(root("dir1\\moved.txt")).unwrap(),
        b"hello after rename"
    );
    assert_eq!(
        vfs_read(&m.vfs, b"/dir1/moved.txt").unwrap(),
        b"hello after rename"
    );
}

#[test]
fn timestamps_update_on_write() {
    let m = mount();
    let p = root("hello.txt");
    let old = SystemTime::UNIX_EPOCH + Duration::from_secs(1_000_000_000);
    let f = OpenOptions::new().write(true).open(&p).unwrap();
    f.set_times(
        fs::FileTimes::new()
            .set_modified(old)
            .set_accessed(old + Duration::from_secs(5)),
    )
    .unwrap();
    drop(f);
    let md = fs::metadata(&p).unwrap();
    assert_eq!(md.modified().unwrap(), old);
    assert_eq!(md.accessed().unwrap(), old + Duration::from_secs(5));
    let (h, st) = m.vfs.open(b"/hello.txt").unwrap();
    m.vfs.close(h);
    assert_eq!(st.mtime.sec, 1_000_000_000);
    // A write moves the modification time to now.
    let before = SystemTime::now() - Duration::from_secs(5);
    let mut f = OpenOptions::new().append(true).open(&p).unwrap();
    f.write_all(b"!").unwrap();
    drop(f);
    std::thread::sleep(Duration::from_millis(1100)); // FileInfoTimeout
    let t = fs::metadata(&p).unwrap().modified().unwrap();
    assert!(t > before, "{t:?} <= {before:?}");
    assert!(t < SystemTime::now() + Duration::from_secs(60));
}

#[test]
fn readonly_attribute_round_trip() {
    let m = mount();
    let p = root("hello.txt");
    let mut perm = fs::metadata(&p).unwrap().permissions();
    perm.set_readonly(true);
    fs::set_permissions(&p, perm.clone()).unwrap();
    assert!(fs::metadata(&p).unwrap().permissions().readonly());
    let (h, st) = m.vfs.open(b"/hello.txt").unwrap();
    m.vfs.close(h);
    assert_eq!(st.mode & 0o222, 0, "mode {:o}", st.mode);
    assert!(OpenOptions::new().write(true).open(&p).is_err());
    #[allow(clippy::permissions_set_readonly_false)]
    perm.set_readonly(false);
    fs::set_permissions(&p, perm).unwrap();
    assert!(!fs::metadata(&p).unwrap().permissions().readonly());
    fs::write(&p, b"writable again").unwrap();
    assert_eq!(vfs_read(&m.vfs, b"/hello.txt").unwrap(), b"writable again");
    // A file created read-only.
    let f = OpenOptions::new()
        .write(true)
        .create_new(true)
        .attributes(0x1) // FILE_ATTRIBUTE_READONLY
        .open(root("ro-new"))
        .unwrap();
    drop(f);
    let (h, st) = m.vfs.open(b"/ro-new").unwrap();
    m.vfs.close(h);
    assert_eq!(st.mode & 0o222, 0, "mode {:o}", st.mode);
}

#[test]
fn unicode_filenames() {
    let m = mount();
    let names_in = [
        "한글 파일.txt",
        "emoji \u{1F600}\u{1F680}.txt",
        "e\u{0301}combining.txt",
        "ﾊﾝｶｸ",
    ];
    for (i, n) in names_in.iter().enumerate() {
        fs::write(root(n), format!("{i}")).unwrap();
    }
    for (i, n) in names_in.iter().enumerate() {
        assert_eq!(fs::read(root(n)).unwrap(), format!("{i}").as_bytes());
        // On disk the name is the UTF-8 encoding, byte for byte.
        let disk = format!("/{n}");
        assert_eq!(
            vfs_read(&m.vfs, disk.as_bytes()).unwrap(),
            format!("{i}").as_bytes(),
            "{n}"
        );
    }
    let listed = names(&root(""));
    for n in names_in {
        assert!(listed.iter().any(|x| x == n), "{n} not in {listed:?}");
    }
    // The precomposed form is a different name.
    assert!(!root("\u{e9}combining.txt").exists());
    fs::create_dir(root("디렉터리")).unwrap();
    fs::rename(root(names_in[0]), root("디렉터리\\옮김")).unwrap();
    assert_eq!(names(&root("디렉터리")), ["옮김"]);
}

#[test]
fn max_name_255() {
    let m = mount();
    let n255 = "a".repeat(255);
    fs::write(root(&n255), b"255").unwrap();
    assert_eq!(fs::read(root(&n255)).unwrap(), b"255");
    assert!(names(&root("")).contains(&n255));
    assert_eq!(
        vfs_read(&m.vfs, format!("/{n255}").as_bytes()).unwrap(),
        b"255"
    );
    let n256 = "b".repeat(256);
    assert!(fs::write(root(&n256), b"256").is_err());
    assert!(fs::create_dir(root(&n256)).is_err());
}

#[test]
fn reserved_chars_roundtrip_via_sfm_mapping() {
    let m = mount();
    // Windows-side names in the private-use area map to the reserved
    // characters on disk.
    fs::write(root("a\u{F03A}b"), b"colon").unwrap();
    fs::write(root("q\u{F03F}"), b"question").unwrap();
    fs::write(root("trail\u{F02E}"), b"dot").unwrap();
    fs::create_dir(root("d\u{F02A}")).unwrap();
    fs::write(root("d\u{F02A}\\in\u{F07C}side"), b"pipe").unwrap();
    assert_eq!(vfs_read(&m.vfs, b"/a:b").unwrap(), b"colon");
    assert_eq!(vfs_read(&m.vfs, b"/q?").unwrap(), b"question");
    assert_eq!(vfs_read(&m.vfs, b"/trail.").unwrap(), b"dot");
    assert_eq!(vfs_read(&m.vfs, b"/d*/in|side").unwrap(), b"pipe");
    // And back: listings show the mapped names.
    let n = names(&root(""));
    for want in ["a\u{F03A}b", "q\u{F03F}", "trail\u{F02E}", "d\u{F02A}"] {
        assert!(n.iter().any(|x| x == want), "{want:?} not in {n:?}");
    }
    fs::rename(root("a\u{F03A}b"), root("c\u{F03C}d")).unwrap();
    assert_eq!(vfs_read(&m.vfs, b"/c<d").unwrap(), b"colon");
    fs::remove_file(root("c\u{F03C}d")).unwrap();
    assert_eq!(m.vfs.open(b"/c<d").map(|_| ()), Err(Errno::ENOENT));
}

#[test]
fn case_only_names_coexist_on_case_sensitive_mount() {
    let m = mount();
    fs::write(root("Foo"), b"upper").unwrap();
    fs::write(root("foo"), b"lower").unwrap();
    assert_eq!(fs::read(root("Foo")).unwrap(), b"upper");
    assert_eq!(fs::read(root("foo")).unwrap(), b"lower");
    assert_eq!(vfs_read(&m.vfs, b"/Foo").unwrap(), b"upper");
    assert_eq!(vfs_read(&m.vfs, b"/foo").unwrap(), b"lower");
    assert!(!root("FOO").exists());
    fs::remove_file(root("foo")).unwrap();
    assert_eq!(fs::read(root("Foo")).unwrap(), b"upper");
}

#[test]
fn case_insensitive_open_and_normalized_names() {
    let m = mount_ci();
    assert_eq!(fs::read(root("HELLO.TXT")).unwrap(), b"hello");
    let f = File::open(root("HELLO.TXT")).unwrap();
    assert_eq!(final_name(&f), "\\hello.txt");
    drop(f);
    let f = File::open(root("DIR1\\Nested.Txt")).unwrap();
    assert_eq!(final_name(&f), "\\dir1\\nested.txt");
    drop(f);
    // Creating an existing name in another case opens (and truncates) it.
    fs::write(root("Hello.txt"), b"replaced").unwrap();
    assert_eq!(names(&root("")), ["dir1", "hello.txt"]);
    assert_eq!(vfs_read(&m.vfs, b"/hello.txt").unwrap(), b"replaced");
    // A new file below a parent spelled differently keeps its own spelling.
    let f = File::create(root("DIR1\\New.Txt")).unwrap();
    assert_eq!(final_name(&f), "\\dir1\\New.Txt");
    drop(f);
    assert!(m.vfs.open(b"/dir1/New.Txt").is_ok());
    assert_eq!(names(&root("dir1")), ["New.Txt", "nested.txt"]);
}

#[test]
fn case_insensitive_rename_and_delete() {
    let m = mount_ci();
    // Case-only rename.
    fs::rename(root("hello.txt"), root("Hello.TXT")).unwrap();
    assert_eq!(names(&root("")), ["Hello.TXT", "dir1"]);
    assert_eq!(vfs_read(&m.vfs, b"/Hello.TXT").unwrap(), b"hello");
    let f = File::open(root("hello.txt")).unwrap();
    assert_eq!(final_name(&f), "\\Hello.TXT");
    drop(f);
    // Rename and delete through other spellings: Cleanup and Rename must
    // see the disk names, not upcased ones.
    fs::rename(root("DIR1\\NESTED.TXT"), root("dir1\\Moved.txt")).unwrap();
    assert_eq!(names(&root("dir1")), ["Moved.txt"]);
    fs::remove_file(root("DIR1\\MOVED.TXT")).unwrap();
    assert!(names(&root("dir1")).is_empty());
    assert_eq!(
        m.vfs.open(b"/dir1/Moved.txt").map(|_| ()),
        Err(Errno::ENOENT)
    );
    fs::remove_dir(root("Dir1")).unwrap();
    assert_eq!(names(&root("")), ["Hello.TXT"]);
    // A collision with another spelling is refused without replace.
    fs::write(root("other"), b"o").unwrap();
    let err = move_file(
        &format!("{DRIVE}\\other"),
        &format!("{DRIVE}\\HELLO.txt"),
        0,
    )
    .unwrap_err();
    assert_eq!(err.kind(), ErrorKind::AlreadyExists, "{err:?}");
    // Non-ASCII case folding.
    fs::write(root("Ärger"), b"umlaut").unwrap();
    assert_eq!(fs::read(root("äRGER")).unwrap(), b"umlaut");
    fs::remove_file(root("ÄRGER")).unwrap();
    assert_eq!(
        m.vfs.open("/Ärger".as_bytes()).map(|_| ()),
        Err(Errno::ENOENT)
    );
}

#[test]
fn unmount_with_open_handle() {
    let mut m = mount();
    let mut f = OpenOptions::new()
        .read(true)
        .write(true)
        .open(root("hello.txt"))
        .unwrap();
    f.write_all(b"J").unwrap();
    let d = OpenOptions::new()
        .read(true)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
        .open(root("dir1"))
        .unwrap();
    let fs = m.fs.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        drop(fs);
        tx.send(()).unwrap();
    });
    rx.recv_timeout(Duration::from_secs(30))
        .expect("MountedFs drop hung with open handles");
    assert!(f.write_all(b"x").and_then(|_| f.sync_all()).is_err());
    let mut buf = [0u8; 4];
    f.seek(SeekFrom::Start(0)).ok();
    assert!(f.read(&mut buf).is_err(), "read through a dead mount");
    drop(d);
    drop(f);
    assert_eq!(Arc::strong_count(&m.vfs), 1);
    // The write made before the unmount reached the Vfs.
    assert_eq!(vfs_read(&m.vfs, b"/hello.txt").unwrap()[0], b'J');
}

#[test]
fn flush_volume_and_file() {
    let _m = mount();
    let f = OpenOptions::new()
        .write(true)
        .open(root("hello.txt"))
        .unwrap();
    f.sync_all().unwrap();
    f.sync_data().unwrap();
    drop(f);
    // Flushing the volume handle (FlushFileBuffers on \\.\Y:) needs
    // administrator rights, which the VM's SSH user has.
    let v = OpenOptions::new()
        .read(true)
        .write(true)
        .open(format!("\\\\.\\{DRIVE}"));
    if let Ok(v) = v {
        v.sync_all().unwrap();
    }
}

#[test]
fn many_files_create_write_delete() {
    let m = mount();
    fs::create_dir(root("many")).unwrap();
    for i in 0..500 {
        fs::write(root(&format!("many\\f{i}")), format!("{i}")).unwrap();
    }
    assert_eq!(names(&root("many")).len(), 500);
    for i in (0..500).step_by(2) {
        fs::remove_file(root(&format!("many\\f{i}"))).unwrap();
    }
    assert_eq!(names(&root("many")).len(), 250);
    assert_eq!(vfs_read(&m.vfs, b"/many/f499").unwrap(), b"499");
    let (h, _) = m.vfs.open(b"/many").unwrap();
    let n = m.vfs.read_dir(h, 0, 1000).unwrap().len();
    m.vfs.close(h);
    assert_eq!(n, 250);
}
