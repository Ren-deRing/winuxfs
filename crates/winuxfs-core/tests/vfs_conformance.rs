//! Path-level `Vfs` over memfs: the shared conformance suite plus Vfs-only
//! checks (handles, case modes, read-only, reference accounting).
mod common;

use common::*;
use std::collections::HashSet;
use std::sync::Arc;
use winuxfs_conformance::*;
use winuxfs_core::plugin::*;
use winuxfs_core::vfs::*;
use winuxfs_core::Errno;

const EBADF: Errno = Errno(9);

fn no_devs() -> Devices {
    Devices {
        data: None,
        log: None,
        rt: None,
    }
}

fn memfs() -> Arc<Mount> {
    let b = load_plugin(&plugin("memfs", "memfs")).unwrap();
    Arc::new(b.mount(no_devs(), "").unwrap())
}

fn opts(case_insensitive: bool, read_only: bool) -> VfsOptions {
    VfsOptions {
        case_insensitive,
        read_only,
    }
}

fn vfs(case_insensitive: bool) -> Vfs {
    Vfs::new(memfs(), opts(case_insensitive, false))
}

/// A private instance of the counting memfs (its counters are per library
/// instance) and accessors for its counters.
struct Counting {
    lib: libloading::Library,
    backend: Backend,
}

impl Counting {
    fn new(tag: &str) -> Counting {
        let p = private_copy(&test_plugin("testplug", "memfs_counting"), tag);
        let backend = load_plugin(&p).unwrap();
        assert_eq!(backend.name(), "memfs_counting");
        // SAFETY: the same library file load_plugin opened; it stays loaded.
        let lib = unsafe { libloading::Library::new(&p) }.unwrap();
        Counting { lib, backend }
    }

    fn mount(&self) -> Arc<Mount> {
        Arc::new(self.backend.mount(no_devs(), "").unwrap())
    }

    fn counter(&self, sym: &[u8]) -> u64 {
        // SAFETY: the symbol is `unsigned long long f(void)` in memfs_counting.c.
        unsafe {
            let f = self.lib.get::<unsafe extern "C" fn() -> u64>(sym).unwrap();
            f()
        }
    }

    fn taken(&self) -> u64 {
        self.counter(b"memfs_counting_refs_taken")
    }

    fn forgotten(&self) -> u64 {
        self.counter(b"memfs_counting_refs_forgotten")
    }

    fn mutations(&self) -> u64 {
        self.counter(b"memfs_counting_mutations")
    }
}

fn read_all(v: &Vfs, h: Handle) -> Vec<u8> {
    let mut buf = vec![0u8; 4096];
    let n = v.read(h, 0, &mut buf).unwrap();
    buf.truncate(n);
    buf
}

fn mkfile(v: &Vfs, p: &[u8], data: &[u8]) -> Stat {
    let (h, st) = v.create(p, FileType::Reg, 0o644).unwrap();
    assert_eq!(v.write(h, 0, data).unwrap(), data.len());
    v.close(h);
    st
}

fn mkdir(v: &Vfs, p: &[u8]) -> Stat {
    let (h, st) = v.create(p, FileType::Dir, 0o755).unwrap();
    v.close(h);
    st
}

fn names(v: &Vfs, p: &[u8]) -> Vec<Vec<u8>> {
    let (h, _) = v.open(p).unwrap();
    let mut out = Vec::new();
    let mut cookie = 0;
    loop {
        let page = v.read_dir(h, cookie, 64).unwrap();
        let Some(last) = page.last() else { break };
        cookie = last.cookie;
        out.extend(page.into_iter().map(|e| e.name));
    }
    v.close(h);
    out.sort();
    out
}

// ---- the shared suite ----

#[test]
fn run_all_default_vfs() {
    let v = vfs(false);
    run_all(&VfsPathFs(&v), "/");
}

#[test]
fn run_all_case_insensitive_vfs() {
    let v = vfs(true);
    run_all(&VfsPathFs(&v), "/");
}

/// Delegates to `VfsPathFs` but breaks `set_len`.
struct BrokenSetLen<'a>(VfsPathFs<'a>);

impl PathFs for BrokenSetLen<'_> {
    fn create_dir(&self, p: &str) -> std::io::Result<()> {
        self.0.create_dir(p)
    }
    fn write_file(&self, p: &str, off: u64, data: &[u8]) -> std::io::Result<()> {
        self.0.write_file(p, off, data)
    }
    fn read_file(&self, p: &str, off: u64, len: usize) -> std::io::Result<Vec<u8>> {
        self.0.read_file(p, off, len)
    }
    fn set_len(&self, _: &str, _: u64) -> std::io::Result<()> {
        Err(std::io::Error::other("broken"))
    }
    fn remove_file(&self, p: &str) -> std::io::Result<()> {
        self.0.remove_file(p)
    }
    fn remove_dir(&self, p: &str) -> std::io::Result<()> {
        self.0.remove_dir(p)
    }
    fn rename(&self, from: &str, to: &str) -> std::io::Result<()> {
        self.0.rename(from, to)
    }
    fn list_dir(&self, p: &str) -> std::io::Result<Vec<String>> {
        self.0.list_dir(p)
    }
    fn is_dir(&self, p: &str) -> std::io::Result<bool> {
        self.0.is_dir(p)
    }
    fn len(&self, p: &str) -> std::io::Result<u64> {
        self.0.len(p)
    }
}

#[test]
fn run_all_reports_failing_test_name() {
    let v = vfs(false);
    let fs = BrokenSetLen(VfsPathFs(&v));
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run_all(&fs, "/")));
    let p = r.expect_err("run_all must fail");
    let msg = p.downcast_ref::<String>().cloned().unwrap_or_default();
    assert!(msg.contains("conf_basic_file_io"), "{msg}");
    assert!(msg.contains("broken"), "{msg}");
}

#[test]
fn conf_case_sensitive_coexist_default_vfs() {
    let v = vfs(false);
    let fs = VfsPathFs(&v);
    fs.create_dir("/cs").unwrap();
    conf_case_sensitive_coexist(&fs, "/cs");
}

#[test]
fn conf_case_insensitive_collision_vfs() {
    let v = vfs(true);
    let fs = VfsPathFs(&v);
    fs.create_dir("/ci").unwrap();
    conf_case_insensitive_collision(&fs, "/ci");

    // Vfs level: create of a case-only-different name is EEXIST, opening
    // with another case opens the stored name's inode.
    let st = mkfile(&v, b"/Foo", b"x");
    assert_eq!(
        v.create(b"/foo", FileType::Reg, 0o644).map(|_| ()),
        Err(Errno::EEXIST)
    );
    assert_eq!(
        v.create(b"/FOO", FileType::Dir, 0o755).map(|_| ()),
        Err(Errno::EEXIST)
    );
    let (h, s2) = v.open(b"/FOO").unwrap();
    assert_eq!(s2.ino, st.ino);
    v.close(h);
    assert_eq!(names(&v, b"/"), vec![b"Foo".to_vec(), b"ci".to_vec()]);
    // Non-ASCII simple folding.
    mkfile(&v, "/Ärger".as_bytes(), b"u");
    let (h, _) = v.open("/äRGER".as_bytes()).unwrap();
    assert_eq!(read_all(&v, h), b"u");
    v.close(h);
    // Rename to a case-only-different name of itself changes the stored name.
    v.rename(b"/foo", b"/fOo", false).unwrap();
    assert!(names(&v, b"/").contains(&b"fOo".to_vec()));
    let (h, s3) = v.open(b"/FOO").unwrap();
    assert_eq!(s3.ino, st.ino);
    v.close(h);
}

#[test]
fn default_vfs_case_only_different_names_coexist() {
    let v = vfs(false);
    let a = mkfile(&v, b"/Foo", b"1");
    let b = mkfile(&v, b"/foo", b"2");
    assert_ne!(a.ino, b.ino);
    assert_eq!(v.open(b"/FOO").map(|_| ()), Err(Errno::ENOENT));
    assert_eq!(names(&v, b"/"), vec![b"Foo".to_vec(), b"foo".to_vec()]);
}

// ---- Vfs-only conformance ----

#[test]
fn conf_unlink_open_file() {
    let v = vfs(false);
    mkfile(&v, b"/u", b"keep me");
    let (h, _) = v.open(b"/u").unwrap();
    v.unlink(b"/u").unwrap();
    assert_eq!(v.open(b"/u").map(|_| ()), Err(Errno::ENOENT));
    assert!(names(&v, b"/").is_empty());
    assert_eq!(read_all(&v, h), b"keep me");
    v.write(h, 7, b"!").unwrap();
    assert_eq!(read_all(&v, h), b"keep me!");
    assert_eq!(v.stat(h).unwrap().nlink, 0);
    v.close(h);
    assert_eq!(v.open(b"/u").map(|_| ()), Err(Errno::ENOENT));
    // A new file with the same name is a different, empty file.
    let (h2, st) = v.create(b"/u", FileType::Reg, 0o644).unwrap();
    assert_eq!(st.size, 0);
    v.close(h2);
}

#[test]
fn rename_open_file_keeps_handle_working() {
    let v = vfs(false);
    mkdir(&v, b"/d");
    mkfile(&v, b"/a", b"one");
    mkfile(&v, b"/d/victim", b"old");
    let (h, st) = v.open(b"/a").unwrap();
    let (hv, _) = v.open(b"/d/victim").unwrap();
    v.rename(b"/a", b"/d/victim", true).unwrap();
    assert_eq!(v.open(b"/a").map(|_| ()), Err(Errno::ENOENT));
    v.write(h, 3, b"two").unwrap();
    let (h2, st2) = v.open(b"/d/victim").unwrap();
    assert_eq!(st2.ino, st.ino);
    assert_eq!(read_all(&v, h2), b"onetwo");
    // The replaced file is still readable through its own handle.
    assert_eq!(read_all(&v, hv), b"old");
    v.close(hv);
    v.close(h2);
    v.close(h);
    // replace = false refuses an existing destination.
    mkfile(&v, b"/b", b"b");
    assert_eq!(v.rename(b"/b", b"/d/victim", false), Err(Errno::EEXIST));
}

#[test]
fn conf_big_dir_paging() {
    let v = vfs(false);
    mkdir(&v, b"/big");
    let mut all = HashSet::new();
    for i in 0..10_000 {
        let n = format!("f{i:05}");
        mkfile(&v, format!("/big/{n}").as_bytes(), b"");
        all.insert(n.into_bytes());
    }
    let (h, _) = v.open(b"/big").unwrap();
    let mut seen = Vec::new();
    let mut deleted = 0;
    let mut cookie = 0;
    loop {
        let page = v.read_dir(h, cookie, 7).unwrap();
        let Some(last) = page.last() else { break };
        assert!(page.len() <= 7);
        cookie = last.cookie;
        for e in &page {
            assert_eq!(e.ftype, FileType::Reg);
            seen.push(e.name.clone());
        }
        // Delete already-read entries (from this and earlier pages).
        while deleted < 1000 && deleted < seen.len() {
            let mut p = b"/big/".to_vec();
            p.extend_from_slice(&seen[deleted]);
            v.unlink(&p).unwrap();
            deleted += 1;
        }
    }
    v.close(h);
    assert_eq!(deleted, 1000);
    let uniq: HashSet<_> = seen.iter().cloned().collect();
    assert_eq!(uniq.len(), seen.len(), "duplicates");
    assert_eq!(seen.len(), 10_000);
    assert_eq!(uniq, all, "missing entries");
    assert_eq!(names(&v, b"/big").len(), 9_000);
}

#[test]
fn conf_name_boundaries_vfs() {
    let v = vfs(false);
    // Non-UTF-8 names round-trip at the Vfs level.
    let st = mkfile(&v, &[b'/', 0x66, 0xFF], b"bytes");
    let (h, s2) = v.open(&[b'/', 0x66, 0xFF]).unwrap();
    assert_eq!(s2.ino, st.ino);
    assert_eq!(read_all(&v, h), b"bytes");
    v.close(h);
    assert_eq!(names(&v, b"/"), vec![vec![0x66, 0xFF]]);
    v.unlink(&[b'/', 0x66, 0xFF]).unwrap();
    // 255 bytes work, 256 are ENAMETOOLONG.
    let mut ok = b"/".to_vec();
    ok.extend(std::iter::repeat_n(b'n', 255));
    mkfile(&v, &ok, b"");
    let mut long = b"/".to_vec();
    long.extend(std::iter::repeat_n(b'n', 256));
    assert_eq!(
        v.create(&long, FileType::Reg, 0o644).map(|_| ()),
        Err(Errno::ENAMETOOLONG)
    );
    assert_eq!(v.open(&long).map(|_| ()), Err(Errno::ENAMETOOLONG));
    // The same holds in case-insensitive mode (non-UTF-8 bytes fold to themselves).
    let ci = vfs(true);
    mkfile(&ci, &[b'/', 0x46, 0xFF], b"F");
    let (h, _) = ci.open(&[b'/', 0x66, 0xFF]).unwrap();
    assert_eq!(read_all(&ci, h), b"F");
    ci.close(h);
    assert_eq!(names(&ci, b"/"), vec![vec![0x46, 0xFF]]);
}

#[test]
fn path_syntax_is_validated() {
    let v = vfs(false);
    mkdir(&v, b"/a");
    for bad in [
        &b""[..],
        b"a",
        b"/a/",
        b"//a",
        b"/a//b",
        b"/.",
        b"/..",
        b"/a/.",
        b"/a/../a",
        b"/./a",
    ] {
        assert_eq!(v.open(bad).map(|_| ()), Err(Errno::EINVAL), "{bad:?}");
        assert_eq!(
            v.create(bad, FileType::Reg, 0o644).map(|_| ()),
            Err(Errno::EINVAL),
            "{bad:?}"
        );
        assert_eq!(v.unlink(bad), Err(Errno::EINVAL), "{bad:?}");
        assert_eq!(v.rmdir(bad), Err(Errno::EINVAL), "{bad:?}");
        assert_eq!(v.rename(bad, b"/z", true), Err(Errno::EINVAL), "{bad:?}");
        assert_eq!(v.rename(b"/a", bad, true), Err(Errno::EINVAL), "{bad:?}");
    }
    // The root itself opens, but is not a name that can be removed or moved.
    let (h, st) = v.open(b"/").unwrap();
    assert_eq!(st.ino, ROOT_INO);
    assert_eq!(st.ftype, FileType::Dir);
    v.close(h);
    assert_eq!(v.rmdir(b"/"), Err(Errno::EINVAL));
    assert_eq!(v.unlink(b"/"), Err(Errno::EINVAL));
    assert_eq!(v.rename(b"/", b"/q", true), Err(Errno::EINVAL));
    assert_eq!(
        v.create(b"/", FileType::Dir, 0o755).map(|_| ()),
        Err(Errno::EEXIST)
    );
}

#[test]
fn create_open_errors() {
    let v = vfs(false);
    assert_eq!(
        v.create(b"/no/f", FileType::Reg, 0o644).map(|_| ()),
        Err(Errno::ENOENT)
    );
    mkfile(&v, b"/f", b"");
    assert_eq!(
        v.create(b"/f", FileType::Reg, 0o644).map(|_| ()),
        Err(Errno::EEXIST)
    );
    assert_eq!(v.open(b"/f/x").map(|_| ()), Err(Errno::ENOTDIR));
    assert_eq!(
        v.create(b"/f/x", FileType::Reg, 0o644).map(|_| ()),
        Err(Errno::ENOTDIR)
    );
    assert_eq!(v.rmdir(b"/f"), Err(Errno::ENOTDIR));
    mkdir(&v, b"/d");
    assert_eq!(v.unlink(b"/d"), Err(Errno::EISDIR));
    let (h, _) = v.open(b"/f").unwrap();
    assert_eq!(v.read_dir(h, 0, 10).map(|_| ()), Err(Errno::ENOTDIR));
    v.close(h);
    // Closed or never-issued handles are EBADF; close of one is a no-op.
    assert_eq!(v.stat(h).map(|_| ()), Err(EBADF));
    assert_eq!(v.read(h, 0, &mut [0u8; 4]), Err(EBADF));
    assert_eq!(v.write(Handle(9999), 0, b"x"), Err(EBADF));
    v.close(h);
    v.close(Handle(12345));
}

#[test]
fn set_len_set_times_statfs() {
    let v = vfs(false);
    mkfile(&v, b"/f", b"0123456789");
    let (h, _) = v.open(b"/f").unwrap();
    v.set_len(h, 4).unwrap();
    assert_eq!(read_all(&v, h), b"0123");
    assert_eq!(v.stat(h).unwrap().size, 4);
    let t = Timespec {
        sec: 1_000_000,
        nsec: 5,
    };
    let before = v.stat(h).unwrap();
    v.set_times(h, None, Some(t)).unwrap();
    let after = v.stat(h).unwrap();
    assert_eq!(after.mtime, t);
    assert_eq!(after.atime, before.atime);
    v.set_times(h, Some(t), None).unwrap();
    assert_eq!(v.stat(h).unwrap().atime, t);
    v.close(h);
    let s = v.statfs().unwrap();
    assert_eq!(s.name_max, 255);
    assert!(s.total_bytes > 0);
}

#[test]
fn handles_are_unique_and_monotonic() {
    let v = vfs(false);
    let st = mkfile(&v, b"/f", b"data");
    let hs: Vec<Handle> = (0..5).map(|_| v.open(b"/f").unwrap().0).collect();
    for w in hs.windows(2) {
        assert!(w[0].0 < w[1].0, "{:?}", hs);
    }
    for h in &hs {
        assert_eq!(v.stat(*h).unwrap().ino, st.ino);
    }
    v.close(hs[0]);
    v.close(hs[3]);
    assert_eq!(read_all(&v, hs[1]), b"data");
    let h = v.open(b"/f").unwrap().0;
    assert!(h.0 > hs[4].0, "handles are never reused");
    for x in [hs[1], hs[2], hs[4], h] {
        v.close(x);
    }
}

#[test]
fn conf_readonly_vfs_never_calls_backend_write() {
    let c = Counting::new("vfs-ro");
    let m = c.mount();
    {
        let w = Vfs::new(m.clone(), opts(false, false));
        mkdir(&w, b"/d");
        mkfile(&w, b"/d/f", b"content");
        mkdir(&w, b"/empty");
    }
    let before = c.mutations();
    assert!(before > 0, "the counter works");
    for ci in [false, true] {
        let v = Vfs::new(m.clone(), opts(ci, true));
        let (h, st) = v.open(b"/d/f").unwrap();
        assert_eq!(st.size, 7);
        assert_eq!(read_all(&v, h), b"content");
        let (dh, _) = v.open(b"/d").unwrap();
        assert_eq!(v.read_dir(dh, 0, 10).unwrap().len(), 1);
        v.statfs().unwrap();
        let rofs = Err(Errno::EROFS);
        assert_eq!(v.create(b"/new", FileType::Reg, 0o644).map(|_| ()), rofs);
        assert_eq!(v.create(b"/nd", FileType::Dir, 0o755).map(|_| ()), rofs);
        assert_eq!(v.create(b"/d/f", FileType::Reg, 0o644).map(|_| ()), rofs);
        assert_eq!(v.write(h, 0, b"x"), Err(Errno::EROFS));
        assert_eq!(v.write(h, 0, b""), Err(Errno::EROFS));
        assert_eq!(v.set_len(h, 0), rofs);
        assert_eq!(v.set_times(h, None, Some(Timespec::default())), rofs);
        assert_eq!(v.unlink(b"/d/f"), rofs);
        assert_eq!(v.unlink(b"/missing"), rofs);
        assert_eq!(v.rmdir(b"/empty"), rofs);
        assert_eq!(v.rename(b"/d/f", b"/g", true), rofs);
        assert_eq!(v.rename(b"/d", b"/e", false), rofs);
        v.close(dh);
        v.close(h);
        assert_eq!(c.mutations(), before, "case_insensitive = {ci}");
    }
    assert_eq!(c.mutations(), before);
}

/// Every reference the backend hands out is forgotten exactly once by the
/// time all handles are closed and the Vfs is dropped (memfs clamps
/// over-forgetting, so only the counters can show a double forget).
#[test]
fn references_balance_after_drop() {
    let c = Counting::new("vfs-refs");
    let m = c.mount();
    for ci in [false, true] {
        let v = Vfs::new(m.clone(), opts(ci, false));
        mkdir(&v, b"/a");
        mkdir(&v, b"/a/b");
        mkfile(&v, b"/a/b/f", b"f");
        mkfile(&v, b"/a/g", b"g");
        // The same inode opened many times, via cached and fresh paths.
        let hs: Vec<Handle> = (0..6).map(|_| v.open(b"/a/b/f").unwrap().0).collect();
        // Unlink and rename while open; rename over an open file.
        let (hg, _) = v.open(b"/a/g").unwrap();
        v.rename(b"/a/b/f", b"/a/g", true).unwrap();
        assert_eq!(read_all(&v, hg), b"g");
        v.unlink(b"/a/g").unwrap();
        assert_eq!(read_all(&v, hs[2]), b"f");
        // Failed lookups and creates take nothing that is not returned.
        assert!(v.open(b"/a/nope/x").is_err());
        assert!(v.create(b"/a/b", FileType::Dir, 0o755).is_err());
        // Directory rename with cached children, then rmdir.
        mkfile(&v, b"/a/b/k", b"k");
        let (hk, _) = v.open(b"/a/b/k").unwrap();
        v.rename(b"/a/b", b"/a/c", false).unwrap();
        let (hk2, _) = v.open(b"/a/c/k").unwrap();
        v.unlink(b"/a/c/k").unwrap();
        v.rmdir(b"/a/c").unwrap();
        v.rmdir(b"/a").unwrap();
        if ci {
            mkfile(&v, b"/Up", b"");
            let (h, _) = v.open(b"/uP").unwrap();
            v.unlink(b"/UP").unwrap();
            v.close(h);
        }
        // Many distinct names (exercises cache eviction).
        mkdir(&v, b"/many");
        for i in 0..5000 {
            let p = format!("/many/{i}");
            mkfile(&v, p.as_bytes(), b"");
            v.close(v.open(p.as_bytes()).unwrap().0);
        }
        for h in hs.into_iter().chain([hg, hk, hk2]) {
            v.close(h);
        }
        drop(v);
        assert_eq!(c.forgotten(), c.taken(), "case_insensitive = {ci}");
        // Clean up for the next round.
        let v = Vfs::new(m.clone(), opts(false, false));
        for i in 0..5000 {
            v.unlink(format!("/many/{i}").as_bytes()).unwrap();
        }
        v.rmdir(b"/many").unwrap();
        for n in names(&v, b"/") {
            let mut p = b"/".to_vec();
            p.extend(n);
            v.unlink(&p).unwrap();
        }
    }
    assert!(c.taken() > 10_000);
    assert_eq!(c.forgotten(), c.taken());
}

#[test]
fn concurrent_threads_share_a_directory() {
    for ci in [false, true] {
        let v = Arc::new(vfs(ci));
        mkdir(&v, b"/shared");
        mkfile(&v, b"/shared/common", b"common");
        let threads: Vec<_> = (0..8)
            .map(|t| {
                let v = v.clone();
                std::thread::spawn(move || {
                    for i in 0..200 {
                        let p = format!("/shared/t{t}-{i}");
                        let (h, _) = v.create(p.as_bytes(), FileType::Reg, 0o644).unwrap();
                        v.write(h, 0, p.as_bytes()).unwrap();
                        v.close(h);
                        let (h, _) = v.open(p.as_bytes()).unwrap();
                        let (hc, _) = v.open(b"/shared/common").unwrap();
                        assert_eq!(read_all(&v, h), p.as_bytes());
                        assert_eq!(read_all(&v, hc), b"common");
                        if i % 2 == 0 {
                            v.unlink(p.as_bytes()).unwrap();
                        }
                        v.close(hc);
                        v.close(h);
                    }
                })
            })
            .collect();
        for t in threads {
            t.join().unwrap();
        }
        let mut want: Vec<Vec<u8>> = (0..8)
            .flat_map(|t| {
                (1..200)
                    .step_by(2)
                    .map(move |i| format!("t{t}-{i}").into_bytes())
            })
            .collect();
        want.push(b"common".to_vec());
        want.sort();
        assert_eq!(names(&v, b"/shared"), want);
    }
}
