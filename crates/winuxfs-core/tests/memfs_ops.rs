//! memfs backend behaviour through the inode-level `Mount` API.
mod common;

use common::*;
use std::collections::HashSet;
use std::sync::Arc;
use winuxfs_abi as abi;
use winuxfs_core::plugin::*;
use winuxfs_core::*;

const EPERM: Errno = Errno(1);

fn memfs() -> Mount {
    let b = load_plugin(&plugin("memfs", "memfs")).unwrap();
    assert_eq!(b.name(), "memfs");
    b.mount(
        Devices {
            data: None,
            log: None,
            rt: None,
        },
        "",
    )
    .unwrap()
}

fn mkfile(m: &Mount, dir: u64, name: &[u8]) -> Stat {
    m.create(dir, name, FileType::Reg, 0o644, 0, 0).unwrap()
}

fn mkdir(m: &Mount, dir: u64, name: &[u8]) -> Stat {
    m.create(dir, name, FileType::Dir, 0o755, 0, 0).unwrap()
}

fn read_all(m: &Mount, ino: u64, off: u64, len: usize) -> Vec<u8> {
    let mut buf = vec![0xAAu8; len];
    let n = m.read(ino, off, &mut buf).unwrap();
    buf.truncate(n);
    buf
}

/// Every entry of `dir`, paging `page` at a time.
fn list(m: &Mount, dir: u64, page: usize) -> Vec<DirEntry> {
    let mut out = Vec::new();
    let mut cookie = 0;
    loop {
        let v = m.readdir(dir, cookie, page).unwrap();
        if v.is_empty() {
            return out;
        }
        cookie = v.last().unwrap().cookie;
        out.extend(v);
    }
}

fn names(m: &Mount, dir: u64) -> Vec<Vec<u8>> {
    let mut v: Vec<_> = list(m, dir, 64).into_iter().map(|e| e.name).collect();
    v.sort();
    v
}

fn size_only(size: u64) -> Stat {
    Stat {
        ino: 0,
        ftype: FileType::Reg,
        mode: 0,
        uid: 0,
        gid: 0,
        nlink: 0,
        size,
        blocks: 0,
        atime: Timespec::default(),
        mtime: Timespec::default(),
        ctime: Timespec::default(),
        btime: Timespec::default(),
    }
}

#[test]
fn backend_descriptor() {
    let b = load_plugin(&plugin("memfs", "memfs")).unwrap();
    assert_eq!(b.caps(), abi::WFS_CAP_HARDLINK | abi::WFS_CAP_SYMLINK);
    // Device-less: never claims a device in `--type auto`.
    let no = Devices {
        data: None,
        log: None,
        rt: None,
    };
    assert_eq!(b.probe(&no), Ok(false));
}

#[test]
fn root_is_dir_ino_1() {
    let m = memfs();
    let st = m.getattr(ROOT_INO).unwrap();
    assert_eq!(st.ino, 1);
    assert_eq!(st.ftype, FileType::Dir);
    assert_eq!(st.nlink, 2);
    assert!(st.mtime.sec > 1_600_000_000, "{:?}", st.mtime);
    assert!(list(&m, ROOT_INO, 10).is_empty(), "no . or .. entries");
}

#[test]
fn create_lookup_getattr() {
    let m = memfs();
    let c = m
        .create(ROOT_INO, b"f", FileType::Reg, 0o644, 1000, 1001)
        .unwrap();
    assert_ne!(c.ino, ROOT_INO);
    let l = m.lookup(ROOT_INO, b"f").unwrap();
    assert_eq!(l.ino, c.ino);
    assert_eq!(l.ftype, FileType::Reg);
    assert_eq!(l.mode, 0o644);
    assert_eq!((l.uid, l.gid, l.nlink, l.size), (1000, 1001, 1, 0));
    let g = m.getattr(c.ino).unwrap();
    assert_eq!(g, l);
    assert!(g.btime.sec > 1_600_000_000 && g.btime.nsec < 1_000_000_000);
}

#[test]
fn write_then_read_with_hole() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"h");
    assert_eq!(m.write(f.ino, 10, b"xyz").unwrap(), 3);
    assert_eq!(m.getattr(f.ino).unwrap().size, 13);
    let d = read_all(&m, f.ino, 0, 64);
    assert_eq!(d.len(), 13);
    assert_eq!(&d[..10], &[0u8; 10]);
    assert_eq!(&d[10..], b"xyz");
}

#[test]
fn read_past_eof_returns_zero_bytes() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"e");
    m.write(f.ino, 0, b"abc").unwrap();
    assert_eq!(read_all(&m, f.ino, 3, 10), b"");
    assert_eq!(read_all(&m, f.ino, 1000, 10), b"");
    assert_eq!(read_all(&m, f.ino, u64::MAX, 10), b"");
    assert_eq!(read_all(&m, f.ino, 1, 10), b"bc");
    // Zero-length IO is a no-op.
    assert_eq!(m.write(f.ino, 50, b"").unwrap(), 0);
    assert_eq!(m.getattr(f.ino).unwrap().size, 3);
}

#[test]
fn truncate_via_setattr() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"t");
    let data: Vec<u8> = (0..10000u32).map(|i| (i % 251) as u8 + 1).collect();
    m.write(f.ino, 0, &data).unwrap();
    m.setattr(f.ino, &size_only(5000), abi::WFS_SET_SIZE)
        .unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().size, 5000);
    m.setattr(f.ino, &size_only(12000), abi::WFS_SET_SIZE)
        .unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().size, 12000);
    let d = read_all(&m, f.ino, 0, 20000);
    assert_eq!(d.len(), 12000);
    assert_eq!(&d[..5000], &data[..5000]);
    assert!(d[5000..].iter().all(|&b| b == 0));
    // Truncate to zero and regrow.
    m.setattr(f.ino, &size_only(0), abi::WFS_SET_SIZE).unwrap();
    m.setattr(f.ino, &size_only(3), abi::WFS_SET_SIZE).unwrap();
    assert_eq!(read_all(&m, f.ino, 0, 10), vec![0, 0, 0]);
    // Directories cannot be truncated.
    let d = mkdir(&m, ROOT_INO, b"d");
    assert_eq!(
        m.setattr(d.ino, &size_only(0), abi::WFS_SET_SIZE),
        Err(Errno::EISDIR)
    );
}

#[test]
fn setattr_mode_owner_times() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"s");
    let mut a = size_only(0);
    a.mode = 0o4751;
    a.uid = 7;
    a.gid = 8;
    a.atime = Timespec { sec: 1, nsec: 2 };
    a.mtime = Timespec {
        sec: 1_000_000_000,
        nsec: 999_999_999,
    };
    let mask = abi::WFS_SET_MODE
        | abi::WFS_SET_UID
        | abi::WFS_SET_GID
        | abi::WFS_SET_ATIME
        | abi::WFS_SET_MTIME;
    m.setattr(f.ino, &a, mask).unwrap();
    let g = m.getattr(f.ino).unwrap();
    assert_eq!((g.mode, g.uid, g.gid), (0o4751, 7, 8));
    assert_eq!(g.atime, a.atime);
    assert_eq!(g.mtime, a.mtime);
    assert!(g.ctime.sec > 1_600_000_000);
    a.mtime.nsec = 1_000_000_000;
    assert_eq!(m.setattr(f.ino, &a, abi::WFS_SET_MTIME), Err(Errno::EINVAL));
    assert_eq!(m.setattr(9999, &a, abi::WFS_SET_MODE), Err(Errno::ENOENT));
}

#[test]
fn mkdir_rmdir_notempty() {
    let m = memfs();
    let d = mkdir(&m, ROOT_INO, b"d");
    assert_eq!(d.nlink, 2);
    assert_eq!(m.getattr(ROOT_INO).unwrap().nlink, 3);
    mkfile(&m, d.ino, b"x");
    assert_eq!(m.rmdir(ROOT_INO, b"d"), Err(Errno::ENOTEMPTY));
    assert_eq!(m.rmdir(d.ino, b"x"), Err(Errno::ENOTDIR));
    assert_eq!(m.unlink(ROOT_INO, b"d"), Err(Errno::EISDIR));
    m.unlink(d.ino, b"x").unwrap();
    m.rmdir(ROOT_INO, b"d").unwrap();
    assert_eq!(m.getattr(ROOT_INO).unwrap().nlink, 2);
    assert_eq!(m.lookup(ROOT_INO, b"d"), Err(Errno::ENOENT));
    // Still referenced by create: creating inside the removed dir fails.
    assert_eq!(
        m.create(d.ino, b"y", FileType::Reg, 0o644, 0, 0),
        Err(Errno::ENOENT)
    );
    m.forget(d.ino, 1);
    assert_eq!(m.getattr(d.ino), Err(Errno::ENOENT));
}

#[test]
fn exists_and_noent_errors() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"a");
    assert_eq!(
        m.create(ROOT_INO, b"a", FileType::Reg, 0o644, 0, 0),
        Err(Errno::EEXIST)
    );
    assert_eq!(
        m.create(ROOT_INO, b"a", FileType::Dir, 0o755, 0, 0),
        Err(Errno::EEXIST)
    );
    assert_eq!(m.lookup(ROOT_INO, b"nope"), Err(Errno::ENOENT));
    assert_eq!(m.unlink(ROOT_INO, b"nope"), Err(Errno::ENOENT));
    assert_eq!(m.rmdir(ROOT_INO, b"nope"), Err(Errno::ENOENT));
    assert_eq!(m.getattr(12345), Err(Errno::ENOENT));
    assert_eq!(m.lookup(f.ino, b"x"), Err(Errno::ENOTDIR));
    assert_eq!(
        m.create(f.ino, b"x", FileType::Reg, 0o644, 0, 0),
        Err(Errno::ENOTDIR)
    );
    assert_eq!(m.readdir(f.ino, 0, 10).err(), Some(Errno::ENOTDIR));
    let mut buf = [0u8; 4];
    assert_eq!(m.read(ROOT_INO, 0, &mut buf), Err(Errno::EISDIR));
    assert_eq!(m.write(ROOT_INO, 0, b"x"), Err(Errno::EISDIR));
}

#[test]
fn invalid_names() {
    let m = memfs();
    for bad in [&b""[..], b".", b"..", b"a/b", b"a\0b", b"/"] {
        assert_eq!(
            m.create(ROOT_INO, bad, FileType::Reg, 0o644, 0, 0),
            Err(Errno::EINVAL),
            "{bad:?}"
        );
        assert_eq!(m.unlink(ROOT_INO, bad), Err(Errno::EINVAL), "{bad:?}");
    }
    mkfile(&m, ROOT_INO, b"ok");
    assert_eq!(
        m.rename(ROOT_INO, b"ok", ROOT_INO, b"..", 0),
        Err(Errno::EINVAL)
    );
    assert_eq!(
        m.rename(ROOT_INO, b"ok", ROOT_INO, b"x/y", 0),
        Err(Errno::EINVAL)
    );
    // Names are bytes: non-UTF-8 round-trips.
    let f = mkfile(&m, ROOT_INO, &[0x66, 0xFF]);
    assert_eq!(m.lookup(ROOT_INO, &[0x66, 0xFF]).unwrap().ino, f.ino);
    assert!(names(&m, ROOT_INO).contains(&vec![0x66, 0xFF]));
}

#[test]
fn rename_replaces_file_but_not_noreplace() {
    let m = memfs();
    let a = mkfile(&m, ROOT_INO, b"a");
    let b = mkfile(&m, ROOT_INO, b"b");
    m.write(a.ino, 0, b"AAA").unwrap();
    m.write(b.ino, 0, b"BB").unwrap();
    assert_eq!(
        m.rename(ROOT_INO, b"a", ROOT_INO, b"b", abi::WFS_RENAME_NOREPLACE),
        Err(Errno::EEXIST)
    );
    assert_eq!(read_all(&m, b.ino, 0, 10), b"BB");
    m.rename(ROOT_INO, b"a", ROOT_INO, b"b", 0).unwrap();
    assert_eq!(m.lookup(ROOT_INO, b"a"), Err(Errno::ENOENT));
    let nb = m.lookup(ROOT_INO, b"b").unwrap();
    assert_eq!(nb.ino, a.ino);
    assert_eq!(read_all(&m, nb.ino, 0, 10), b"AAA");
    // The replaced inode is unlinked but still referenced by `create`.
    assert_eq!(m.getattr(b.ino).unwrap().nlink, 0);
    m.forget(b.ino, 1);
    assert_eq!(m.getattr(b.ino), Err(Errno::ENOENT));
    // NOREPLACE to a free name works, and moves across directories.
    let d = mkdir(&m, ROOT_INO, b"d");
    m.rename(ROOT_INO, b"b", d.ino, b"c", abi::WFS_RENAME_NOREPLACE)
        .unwrap();
    assert_eq!(m.lookup(d.ino, b"c").unwrap().ino, a.ino);
    assert_eq!(names(&m, ROOT_INO), vec![b"d".to_vec()]);
    // Renaming onto itself (same inode) is a no-op.
    m.rename(d.ino, b"c", d.ino, b"c", 0).unwrap();
    assert_eq!(m.lookup(d.ino, b"c").unwrap().ino, a.ino);
    assert_eq!(
        m.rename(d.ino, b"zz", ROOT_INO, b"q", 0),
        Err(Errno::ENOENT)
    );
    assert_eq!(
        m.rename(d.ino, b"c", ROOT_INO, b"q", abi::WFS_RENAME_EXCHANGE),
        Err(Errno::EINVAL)
    );
}

#[test]
fn rename_dir_type_rules() {
    let m = memfs();
    let d1 = mkdir(&m, ROOT_INO, b"d1");
    let d2 = mkdir(&m, ROOT_INO, b"d2");
    mkfile(&m, ROOT_INO, b"f");
    assert_eq!(
        m.rename(ROOT_INO, b"d1", ROOT_INO, b"f", 0),
        Err(Errno::ENOTDIR)
    );
    assert_eq!(
        m.rename(ROOT_INO, b"f", ROOT_INO, b"d1", 0),
        Err(Errno::EISDIR)
    );
    mkfile(&m, d2.ino, b"child");
    assert_eq!(
        m.rename(ROOT_INO, b"d1", ROOT_INO, b"d2", 0),
        Err(Errno::ENOTEMPTY)
    );
    m.unlink(d2.ino, b"child").unwrap();
    assert_eq!(m.getattr(ROOT_INO).unwrap().nlink, 4);
    m.rename(ROOT_INO, b"d1", ROOT_INO, b"d2", 0).unwrap();
    assert_eq!(m.lookup(ROOT_INO, b"d2").unwrap().ino, d1.ino);
    assert_eq!(m.getattr(ROOT_INO).unwrap().nlink, 3);
    // Move a dir with children into another dir: parent link counts follow.
    let p = mkdir(&m, ROOT_INO, b"p");
    mkfile(&m, d1.ino, b"kid");
    m.rename(ROOT_INO, b"d2", p.ino, b"moved", 0).unwrap();
    assert_eq!(m.getattr(p.ino).unwrap().nlink, 3);
    assert_eq!(m.getattr(ROOT_INO).unwrap().nlink, 3);
    assert_eq!(names(&m, d1.ino), vec![b"kid".to_vec()]);
}

#[test]
fn rename_dir_into_own_subtree_is_einval() {
    let m = memfs();
    let a = mkdir(&m, ROOT_INO, b"a");
    let b = mkdir(&m, a.ino, b"b");
    let c = mkdir(&m, b.ino, b"c");
    assert_eq!(m.rename(ROOT_INO, b"a", c.ino, b"x", 0), Err(Errno::EINVAL));
    assert_eq!(m.rename(ROOT_INO, b"a", a.ino, b"x", 0), Err(Errno::EINVAL));
    assert_eq!(m.rename(a.ino, b"b", b.ino, b"x", 0), Err(Errno::EINVAL));
    // Moving the other way (up the tree) is fine.
    m.rename(b.ino, b"c", ROOT_INO, b"c", 0).unwrap();
    assert_eq!(m.lookup(ROOT_INO, b"c").unwrap().ino, c.ino);
}

#[test]
fn hardlink_nlink_counts() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"f");
    m.write(f.ino, 0, b"data").unwrap();
    let d = mkdir(&m, ROOT_INO, b"d");
    m.link(f.ino, ROOT_INO, b"g").unwrap();
    m.link(f.ino, d.ino, b"h").unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().nlink, 3);
    assert_eq!(m.lookup(d.ino, b"h").unwrap().ino, f.ino);
    assert_eq!(m.link(f.ino, ROOT_INO, b"g"), Err(Errno::EEXIST));
    assert_eq!(m.link(d.ino, ROOT_INO, b"dl"), Err(EPERM));
    m.unlink(ROOT_INO, b"f").unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().nlink, 2);
    m.unlink(d.ino, b"h").unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().nlink, 1);
    assert_eq!(read_all(&m, f.ino, 0, 10), b"data");
    m.unlink(ROOT_INO, b"g").unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().nlink, 0);
    // nlink 0 and still referenced: cannot be linked back.
    assert_eq!(m.link(f.ino, ROOT_INO, b"back"), Err(Errno::ENOENT));
}

#[test]
fn symlink_and_readlink() {
    let m = memfs();
    let s = m.symlink(ROOT_INO, b"l", b"../target", 5, 6).unwrap();
    assert_eq!(s.ftype, FileType::Lnk);
    assert_eq!((s.size, s.uid, s.gid), (9, 5, 6));
    assert_eq!(m.readlink(s.ino).unwrap(), b"../target");
    assert_eq!(m.lookup(ROOT_INO, b"l").unwrap().ino, s.ino);
    assert_eq!(m.symlink(ROOT_INO, b"l", b"x", 0, 0), Err(Errno::EEXIST));
    assert_eq!(m.symlink(ROOT_INO, b"e", b"", 0, 0), Err(Errno::ENOENT));
    let f = mkfile(&m, ROOT_INO, b"f");
    assert_eq!(m.readlink(f.ino), Err(Errno::EINVAL));
    let mut buf = [0u8; 4];
    assert_eq!(m.read(s.ino, 0, &mut buf), Err(Errno::EINVAL));
}

#[test]
fn unlink_while_referenced_keeps_data() {
    let m = memfs();
    let c = mkfile(&m, ROOT_INO, b"u");
    m.forget(c.ino, 1); // drop the create reference
    m.write(c.ino, 0, b"keep me").unwrap();
    let l = m.lookup(ROOT_INO, b"u").unwrap();
    m.unlink(ROOT_INO, b"u").unwrap();
    assert_eq!(m.lookup(ROOT_INO, b"u"), Err(Errno::ENOENT));
    assert_eq!(read_all(&m, l.ino, 0, 64), b"keep me");
    m.write(l.ino, 7, b"!").unwrap();
    assert_eq!(read_all(&m, l.ino, 0, 64), b"keep me!");
    assert_eq!(m.getattr(l.ino).unwrap().nlink, 0);
    m.forget(l.ino, 1);
    assert_eq!(m.getattr(l.ino), Err(Errno::ENOENT));
    // A new file never reuses that inode number.
    let n = mkfile(&m, ROOT_INO, b"u");
    assert_ne!(n.ino, l.ino);
}

#[test]
fn forget_counts_references() {
    let m = memfs();
    let c = mkfile(&m, ROOT_INO, b"r");
    m.lookup(ROOT_INO, b"r").unwrap();
    m.lookup(ROOT_INO, b"r").unwrap();
    m.unlink(ROOT_INO, b"r").unwrap();
    m.forget(c.ino, 2);
    assert!(m.getattr(c.ino).is_ok(), "one reference left");
    m.forget(c.ino, 1);
    assert_eq!(m.getattr(c.ino), Err(Errno::ENOENT));
    // Over-forgetting and forgetting root or unknown inodes is harmless.
    m.forget(c.ino, 5);
    m.forget(ROOT_INO, 100);
    assert!(m.getattr(ROOT_INO).is_ok());
}

#[test]
fn readdir_cookie_survives_deletion() {
    let m = memfs();
    let d = mkdir(&m, ROOT_INO, b"big");
    let mut all = HashSet::new();
    for i in 0..100 {
        let n = format!("f{i:03}").into_bytes();
        let s = mkfile(&m, d.ino, &n);
        m.forget(s.ino, 1);
        all.insert(n);
    }
    let mut seen = Vec::new();
    let mut cookie = 0;
    loop {
        let page = m.readdir(d.ino, cookie, 10).unwrap();
        if page.is_empty() {
            break;
        }
        assert!(page.len() <= 10);
        cookie = page.last().unwrap().cookie;
        for e in &page {
            assert_eq!(e.ftype, FileType::Reg);
            seen.push(e.name.clone());
        }
        // Delete everything read so far in this page, plus an earlier entry.
        for e in &page {
            m.unlink(d.ino, &e.name).unwrap();
        }
    }
    let uniq: HashSet<_> = seen.iter().cloned().collect();
    assert_eq!(uniq.len(), seen.len(), "duplicates");
    assert_eq!(uniq, all, "missing entries");
    assert!(list(&m, d.ino, 10).is_empty());
}

#[test]
fn readdir_cookie_resumes_after_mixed_changes() {
    let m = memfs();
    for i in 0..20 {
        mkfile(&m, ROOT_INO, format!("n{i}").as_bytes());
    }
    let first = m.readdir(ROOT_INO, 0, 5).unwrap();
    // Delete a not-yet-read entry and one already read; add a new one.
    m.unlink(ROOT_INO, b"n10").unwrap();
    m.unlink(ROOT_INO, b"n0").unwrap();
    mkfile(&m, ROOT_INO, b"late");
    let rest = list_from(&m, ROOT_INO, first.last().unwrap().cookie);
    let mut got: Vec<_> = first
        .iter()
        .chain(rest.iter())
        .map(|e| e.name.clone())
        .collect();
    got.sort();
    let mut want: Vec<Vec<u8>> = (0..20)
        .filter(|&i| i != 10)
        .map(|i| format!("n{i}").into_bytes())
        .collect();
    want.push(b"late".to_vec());
    want.sort();
    assert_eq!(got, want);
}

fn list_from(m: &Mount, dir: u64, mut cookie: u64) -> Vec<DirEntry> {
    let mut out = Vec::new();
    loop {
        let v = m.readdir(dir, cookie, 3).unwrap();
        if v.is_empty() {
            return out;
        }
        cookie = v.last().unwrap().cookie;
        out.extend(v);
    }
}

#[test]
fn name_too_long() {
    let m = memfs();
    let long = vec![b'a'; 256];
    assert_eq!(
        m.create(ROOT_INO, &long, FileType::Reg, 0o644, 0, 0),
        Err(Errno::ENAMETOOLONG)
    );
    assert_eq!(m.lookup(ROOT_INO, &long), Err(Errno::ENAMETOOLONG));
    let ok = vec![b'a'; 255];
    let s = mkfile(&m, ROOT_INO, &ok);
    assert_eq!(m.lookup(ROOT_INO, &ok).unwrap().ino, s.ino);
    assert_eq!(names(&m, ROOT_INO), vec![ok]);
}

#[test]
fn big_offsets_and_efbig() {
    let m = memfs();
    let f = mkfile(&m, ROOT_INO, b"big");
    let off = 1u64 << 40;
    assert_eq!(m.write(f.ino, off, b"end").unwrap(), 3);
    let st = m.getattr(f.ino).unwrap();
    assert_eq!(st.size, off + 3);
    assert!(st.blocks * 512 <= 64 * 1024, "sparse: {} blocks", st.blocks);
    assert_eq!(read_all(&m, f.ino, off - 5, 8), b"\0\0\0\0\0end");
    assert_eq!(read_all(&m, f.ino, 12345, 4), vec![0; 4]);
    let max = 1u64 << 41;
    assert_eq!(m.write(f.ino, max, b"x"), Err(Errno::EFBIG));
    assert_eq!(m.write(f.ino, u64::MAX - 1, b"xyz"), Err(Errno::EFBIG));
    // A write straddling the limit is cut short, like Linux.
    assert_eq!(m.write(f.ino, max - 2, b"xyz").unwrap(), 2);
    assert_eq!(m.getattr(f.ino).unwrap().size, max);
    assert_eq!(
        m.setattr(f.ino, &size_only(max + 1), abi::WFS_SET_SIZE),
        Err(Errno::EFBIG)
    );
    // Truncating away the sparse tail releases its space.
    let before = m.statfs().unwrap().free_bytes;
    m.setattr(f.ino, &size_only(0), abi::WFS_SET_SIZE).unwrap();
    assert!(m.statfs().unwrap().free_bytes > before);
    assert_eq!(m.getattr(f.ino).unwrap().blocks, 0);
}

#[test]
fn statfs_reports_cap_and_usage() {
    let m = memfs();
    let s = m.statfs().unwrap();
    assert_eq!(s.total_bytes, 1 << 30);
    assert_eq!(s.free_bytes, s.total_bytes);
    assert_eq!(s.avail_bytes, s.free_bytes);
    assert_eq!(s.name_max, 255);
    assert!(s.block_size > 0);
    let f = mkfile(&m, ROOT_INO, b"x");
    m.write(f.ino, 0, &vec![1u8; 100_000]).unwrap();
    let s2 = m.statfs().unwrap();
    assert!(s2.free_bytes <= s.free_bytes - 100_000);
    m.unlink(ROOT_INO, b"x").unwrap();
    m.forget(f.ino, 1);
    assert_eq!(m.statfs().unwrap().free_bytes, s.total_bytes);
    m.sync().unwrap();
    m.fsync(ROOT_INO, false).unwrap();
}

#[test]
fn concurrent_creates_distinct_names() {
    let m = Arc::new(memfs());
    let threads: Vec<_> = (0..8)
        .map(|t| {
            let m = m.clone();
            std::thread::spawn(move || {
                for i in 0..500 {
                    let s = m
                        .create(
                            ROOT_INO,
                            format!("t{t}-{i}").as_bytes(),
                            FileType::Reg,
                            0o644,
                            0,
                            0,
                        )
                        .unwrap();
                    m.write(s.ino, 0, &[t as u8; 16]).unwrap();
                    m.forget(s.ino, 1);
                }
            })
        })
        .collect();
    for t in threads {
        t.join().unwrap();
    }
    let ents = list(&m, ROOT_INO, 97);
    assert_eq!(ents.len(), 4000);
    let uniq: HashSet<_> = ents.iter().map(|e| e.name.clone()).collect();
    assert_eq!(uniq.len(), 4000);
    let inos: HashSet<_> = ents.iter().map(|e| e.ino).collect();
    assert_eq!(inos.len(), 4000);
}

#[test]
fn case_only_different_names_coexist() {
    let m = memfs();
    let a = mkfile(&m, ROOT_INO, b"a");
    let b = mkfile(&m, ROOT_INO, b"A");
    assert_ne!(a.ino, b.ino);
    assert_eq!(m.lookup(ROOT_INO, b"a").unwrap().ino, a.ino);
    assert_eq!(m.lookup(ROOT_INO, b"A").unwrap().ino, b.ino);
    assert_eq!(names(&m, ROOT_INO), vec![b"A".to_vec(), b"a".to_vec()]);
    m.unlink(ROOT_INO, b"a").unwrap();
    assert_eq!(m.lookup(ROOT_INO, b"A").unwrap().ino, b.ino);
}

#[test]
fn rename_between_hardlinks_is_a_noop() {
    let m = memfs();
    let d = mkdir(&m, ROOT_INO, b"d");
    let f = mkfile(&m, ROOT_INO, b"one");
    m.link(f.ino, d.ino, b"two").unwrap();
    assert_eq!(m.getattr(f.ino).unwrap().nlink, 2);
    m.rename(ROOT_INO, b"one", d.ino, b"two", 0).unwrap();
    // Both names survive and still refer to the same inode.
    assert_eq!(m.lookup(ROOT_INO, b"one").unwrap().ino, f.ino);
    assert_eq!(m.lookup(d.ino, b"two").unwrap().ino, f.ino);
    assert_eq!(m.getattr(f.ino).unwrap().nlink, 2);
    assert_eq!(names(&m, d.ino), vec![b"two".to_vec()]);
}
