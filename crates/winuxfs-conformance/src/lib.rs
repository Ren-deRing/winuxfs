//! Shared path-level conformance suite.
//!
//! The same assertions run against the core's [`Vfs`] (through [`VfsPathFs`])
//! and, on Windows, against `std::fs` on a mounted drive (Task 13). The crate
//! is std-only so it builds for both Linux and Windows targets.
//!
//! Each `conf_*` function takes a filesystem and an existing, empty directory
//! to work in, and panics on failure. [`run_all`] runs every function that
//! holds for any conforming filesystem, each in its own fresh directory.
//! [`conf_case_sensitive_coexist`] and [`conf_case_insensitive_collision`]
//! depend on the filesystem's case mode and are therefore not part of
//! `run_all`; callers run the one matching their configuration.

use std::io::{self, ErrorKind};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};
use winuxfs_core::plugin::FileType;
use winuxfs_core::vfs::{Handle, Vfs};
use winuxfs_core::Errno;

/// Path-level filesystem operations used by the suite. Paths use `/` as the
/// separator. `write_file` creates the file if it does not exist (like
/// `OpenOptions::new().write(true).create(true)`), and `rename` replaces an
/// existing destination file (like `std::fs::rename`).
pub trait PathFs {
    fn create_dir(&self, p: &str) -> io::Result<()>;
    fn write_file(&self, p: &str, off: u64, data: &[u8]) -> io::Result<()>;
    fn read_file(&self, p: &str, off: u64, len: usize) -> io::Result<Vec<u8>>;
    fn set_len(&self, p: &str, len: u64) -> io::Result<()>;
    fn remove_file(&self, p: &str) -> io::Result<()>;
    fn remove_dir(&self, p: &str) -> io::Result<()>;
    fn rename(&self, from: &str, to: &str) -> io::Result<()>;
    fn list_dir(&self, p: &str) -> io::Result<Vec<String>>;
    fn is_dir(&self, p: &str) -> io::Result<bool>;
    fn len(&self, p: &str) -> io::Result<u64>;
}

type ConfFn = fn(&dyn PathFs, &str);

/// Every case-mode-independent conformance function, by name.
pub const ALL: &[(&str, ConfFn)] = &[
    ("conf_basic_file_io", conf_basic_file_io),
    ("conf_nested_dirs", conf_nested_dirs),
    (
        "conf_rename_file_over_existing",
        conf_rename_file_over_existing,
    ),
    (
        "conf_rename_dir_with_children",
        conf_rename_dir_with_children,
    ),
    (
        "conf_remove_nonempty_dir_fails",
        conf_remove_nonempty_dir_fails,
    ),
    (
        "conf_missing_parent_is_notfound",
        conf_missing_parent_is_notfound,
    ),
    ("conf_big_dir_listing", conf_big_dir_listing),
    ("conf_name_boundaries", conf_name_boundaries),
    ("conf_big_offsets", conf_big_offsets),
];

/// `dir` + `/` + `name`, without doubling a trailing separator.
pub fn join(dir: &str, name: &str) -> String {
    if dir.ends_with('/') || dir.ends_with('\\') {
        format!("{dir}{name}")
    } else {
        format!("{dir}/{name}")
    }
}

/// A directory name not used before by this process (pid, time, counter).
pub fn unique_name(prefix: &str) -> String {
    static N: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let n = N.fetch_add(1, Ordering::Relaxed);
    format!("{prefix}-{}-{nanos}-{n}", std::process::id())
}

/// Creates a fresh uniquely named directory under `root` and runs every
/// function in [`ALL`] there, each in its own subdirectory named after it.
/// Panics with the failing test's name in the message.
pub fn run_all(fs: &dyn PathFs, root: &str) {
    let base = join(root, &unique_name("winuxfs-conf"));
    fs.create_dir(&base)
        .unwrap_or_else(|e| panic!("run_all: cannot create {base}: {e}"));
    for (name, f) in ALL {
        let dir = join(&base, name);
        let r = catch_unwind(AssertUnwindSafe(|| {
            fs.create_dir(&dir)
                .unwrap_or_else(|e| panic!("cannot create {dir}: {e}"));
            f(fs, &dir);
        }));
        if let Err(p) = r {
            let msg = p
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| p.downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_default();
            panic!("conformance test {name} failed: {msg}");
        }
    }
}

fn kind<T: std::fmt::Debug>(r: io::Result<T>) -> ErrorKind {
    match r {
        Ok(v) => panic!("expected an error, got Ok({v:?})"),
        Err(e) => e.kind(),
    }
}

fn sorted(mut v: Vec<String>) -> Vec<String> {
    v.sort();
    v
}

/// Create, write, read, overwrite, size changes, delete.
pub fn conf_basic_file_io(fs: &dyn PathFs, dir: &str) {
    let p = join(dir, "f");
    fs.write_file(&p, 0, b"hello world").unwrap();
    assert_eq!(fs.len(&p).unwrap(), 11);
    assert!(!fs.is_dir(&p).unwrap());
    assert_eq!(fs.read_file(&p, 0, 64).unwrap(), b"hello world");
    assert_eq!(fs.read_file(&p, 6, 5).unwrap(), b"world");
    assert_eq!(fs.read_file(&p, 100, 5).unwrap(), b"");
    fs.write_file(&p, 6, b"WORLD!").unwrap();
    assert_eq!(fs.len(&p).unwrap(), 12);
    assert_eq!(fs.read_file(&p, 0, 64).unwrap(), b"hello WORLD!");
    fs.set_len(&p, 5).unwrap();
    assert_eq!(fs.read_file(&p, 0, 64).unwrap(), b"hello");
    fs.set_len(&p, 8).unwrap();
    assert_eq!(fs.read_file(&p, 0, 64).unwrap(), b"hello\0\0\0");
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["f".to_string()]);
    fs.remove_file(&p).unwrap();
    assert_eq!(kind(fs.read_file(&p, 0, 1)), ErrorKind::NotFound);
    assert!(fs.list_dir(dir).unwrap().is_empty());
}

/// A chain of 20 nested directories; every level lists exactly its child.
pub fn conf_nested_dirs(fs: &dyn PathFs, dir: &str) {
    let mut levels = vec![dir.to_string()];
    for i in 0..20 {
        let p = join(levels.last().unwrap(), &format!("d{i}"));
        fs.create_dir(&p).unwrap();
        assert!(fs.is_dir(&p).unwrap());
        levels.push(p);
    }
    let leaf = join(levels.last().unwrap(), "leaf");
    fs.write_file(&leaf, 0, b"deep").unwrap();
    assert_eq!(fs.read_file(&leaf, 0, 10).unwrap(), b"deep");
    for (i, p) in levels.iter().enumerate() {
        let want = if i == 20 {
            "leaf".to_string()
        } else {
            format!("d{i}")
        };
        assert_eq!(fs.list_dir(p).unwrap(), vec![want], "listing {p}");
    }
    assert_eq!(
        kind(fs.create_dir(&levels[5])),
        ErrorKind::AlreadyExists,
        "mkdir over an existing dir"
    );
}

/// Renaming a file onto an existing file replaces it.
pub fn conf_rename_file_over_existing(fs: &dyn PathFs, dir: &str) {
    let (a, b) = (join(dir, "a"), join(dir, "b"));
    fs.write_file(&a, 0, b"AAA").unwrap();
    fs.write_file(&b, 0, b"BB").unwrap();
    fs.rename(&a, &b).unwrap();
    assert_eq!(fs.read_file(&b, 0, 10).unwrap(), b"AAA");
    assert_eq!(fs.len(&b).unwrap(), 3);
    assert_eq!(kind(fs.read_file(&a, 0, 1)), ErrorKind::NotFound);
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["b".to_string()]);
    // And to a free name.
    let c = join(dir, "c");
    fs.rename(&b, &c).unwrap();
    assert_eq!(fs.read_file(&c, 0, 10).unwrap(), b"AAA");
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["c".to_string()]);
}

/// A renamed directory keeps its children readable at the new path.
pub fn conf_rename_dir_with_children(fs: &dyn PathFs, dir: &str) {
    let d = join(dir, "d");
    let sub = join(&d, "sub");
    fs.create_dir(&d).unwrap();
    fs.create_dir(&sub).unwrap();
    fs.write_file(&join(&sub, "x"), 0, b"child").unwrap();
    fs.write_file(&join(&d, "y"), 0, b"y").unwrap();
    let e = join(dir, "e");
    fs.rename(&d, &e).unwrap();
    assert_eq!(kind(fs.is_dir(&d)), ErrorKind::NotFound);
    assert_eq!(
        kind(fs.read_file(&join(&sub, "x"), 0, 1)),
        ErrorKind::NotFound
    );
    assert!(fs.is_dir(&e).unwrap());
    assert_eq!(
        sorted(fs.list_dir(&e).unwrap()),
        vec!["sub".to_string(), "y".to_string()]
    );
    let x = join(&join(&e, "sub"), "x");
    assert_eq!(fs.read_file(&x, 0, 10).unwrap(), b"child");
    fs.write_file(&x, 5, b"!").unwrap();
    assert_eq!(fs.read_file(&x, 0, 10).unwrap(), b"child!");
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["e".to_string()]);
}

/// `remove_dir` of a non-empty directory fails with `DirectoryNotEmpty`.
pub fn conf_remove_nonempty_dir_fails(fs: &dyn PathFs, dir: &str) {
    let d = join(dir, "d");
    fs.create_dir(&d).unwrap();
    let f = join(&d, "f");
    fs.write_file(&f, 0, b"x").unwrap();
    assert_eq!(kind(fs.remove_dir(&d)), ErrorKind::DirectoryNotEmpty);
    assert!(fs.is_dir(&d).unwrap());
    assert_eq!(fs.read_file(&f, 0, 1).unwrap(), b"x");
    fs.remove_file(&f).unwrap();
    fs.remove_dir(&d).unwrap();
    assert_eq!(kind(fs.is_dir(&d)), ErrorKind::NotFound);
}

/// Operations below a missing directory report `NotFound`.
pub fn conf_missing_parent_is_notfound(fs: &dyn PathFs, dir: &str) {
    let missing = join(dir, "nope");
    let f = join(&missing, "f");
    assert_eq!(kind(fs.write_file(&f, 0, b"x")), ErrorKind::NotFound);
    assert_eq!(
        kind(fs.create_dir(&join(&missing, "d"))),
        ErrorKind::NotFound
    );
    assert_eq!(kind(fs.read_file(&f, 0, 1)), ErrorKind::NotFound);
    assert_eq!(kind(fs.list_dir(&missing)), ErrorKind::NotFound);
    assert_eq!(kind(fs.len(&f)), ErrorKind::NotFound);
    assert_eq!(kind(fs.remove_file(&f)), ErrorKind::NotFound);
    assert_eq!(kind(fs.rename(&f, &join(dir, "g"))), ErrorKind::NotFound);
    let real = join(dir, "real");
    fs.write_file(&real, 0, b"r").unwrap();
    assert_eq!(kind(fs.rename(&real, &f)), ErrorKind::NotFound);
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["real".to_string()]);
}

/// A 10,000-entry directory lists every name exactly once.
pub fn conf_big_dir_listing(fs: &dyn PathFs, dir: &str) {
    let mut want: Vec<String> = (0..10_000).map(|i| format!("f{i:05}")).collect();
    for n in &want {
        fs.write_file(&join(dir, n), 0, b"").unwrap();
    }
    let got = sorted(fs.list_dir(dir).unwrap());
    want.sort();
    assert_eq!(got.len(), 10_000);
    assert_eq!(got, want);
}

/// A 255-byte name works; a 256-byte name is rejected (any error kind:
/// platforms differ).
pub fn conf_name_boundaries(fs: &dyn PathFs, dir: &str) {
    let ok = "n".repeat(255);
    let p = join(dir, &ok);
    fs.write_file(&p, 0, b"255").unwrap();
    assert_eq!(fs.read_file(&p, 0, 10).unwrap(), b"255");
    let okd = "d".repeat(255);
    fs.create_dir(&join(dir, &okd)).unwrap();
    assert_eq!(
        sorted(fs.list_dir(dir).unwrap()),
        vec![okd.clone(), ok.clone()]
    );
    let long = "x".repeat(256);
    assert!(fs.write_file(&join(dir, &long), 0, b"x").is_err());
    assert!(fs.create_dir(&join(dir, &long)).is_err());
    assert!(fs.read_file(&join(dir, &long), 0, 1).is_err());
    assert_eq!(fs.list_dir(dir).unwrap().len(), 2);
}

/// A write at offset 2^40 gives size 2^40 + n with zeros everywhere else.
pub fn conf_big_offsets(fs: &dyn PathFs, dir: &str) {
    let p = join(dir, "big");
    let off = 1u64 << 40;
    fs.write_file(&p, off, b"tail").unwrap();
    assert_eq!(fs.len(&p).unwrap(), off + 4);
    assert_eq!(fs.read_file(&p, 0, 16).unwrap(), vec![0u8; 16]);
    assert_eq!(fs.read_file(&p, 1 << 30, 16).unwrap(), vec![0u8; 16]);
    assert_eq!(fs.read_file(&p, off - 4, 16).unwrap(), b"\0\0\0\0tail");
    assert_eq!(fs.read_file(&p, off + 4, 16).unwrap(), b"");
}

/// Case-sensitive filesystems only: `Foo` and `foo` are different files.
pub fn conf_case_sensitive_coexist(fs: &dyn PathFs, dir: &str) {
    let (upper, lower) = (join(dir, "Foo"), join(dir, "foo"));
    fs.write_file(&upper, 0, b"upper").unwrap();
    fs.write_file(&lower, 0, b"lower").unwrap();
    assert_eq!(fs.read_file(&upper, 0, 10).unwrap(), b"upper");
    assert_eq!(fs.read_file(&lower, 0, 10).unwrap(), b"lower");
    assert_eq!(
        kind(fs.read_file(&join(dir, "FOO"), 0, 1)),
        ErrorKind::NotFound
    );
    assert_eq!(
        sorted(fs.list_dir(dir).unwrap()),
        vec!["Foo".to_string(), "foo".to_string()]
    );
    fs.remove_file(&lower).unwrap();
    assert_eq!(fs.read_file(&upper, 0, 10).unwrap(), b"upper");
}

/// Case-insensitive filesystems only: names differing in case collide, and
/// the name as first created is what listings report.
pub fn conf_case_insensitive_collision(fs: &dyn PathFs, dir: &str) {
    let upper = join(dir, "Foo");
    fs.write_file(&upper, 0, b"data").unwrap();
    assert_eq!(
        kind(fs.create_dir(&join(dir, "foo"))),
        ErrorKind::AlreadyExists
    );
    assert_eq!(fs.read_file(&join(dir, "FOO"), 0, 10).unwrap(), b"data");
    fs.write_file(&join(dir, "fOO"), 0, b"DATA").unwrap();
    assert_eq!(fs.read_file(&upper, 0, 10).unwrap(), b"DATA");
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["Foo".to_string()]);
    let d = join(dir, "Dir");
    fs.create_dir(&d).unwrap();
    fs.write_file(&join(dir, "dIR/Inner"), 0, b"in").unwrap();
    assert_eq!(fs.read_file(&join(&d, "inner"), 0, 10).unwrap(), b"in");
    assert_eq!(fs.list_dir(&d).unwrap(), vec!["Inner".to_string()]);
    fs.remove_file(&join(dir, "DIR/INNER")).unwrap();
    fs.remove_dir(&join(dir, "dir")).unwrap();
    assert_eq!(fs.list_dir(dir).unwrap(), vec!["Foo".to_string()]);
}

/// Maps a Linux errno to a portable `io::Error` (by kind, not by raw OS
/// code, whose numbering differs on Windows).
pub fn errno_to_io(e: Errno) -> io::Error {
    io::Error::new(e.io_kind(), format!("errno {}", e.0))
}

/// [`PathFs`] over a [`Vfs`]; paths are UTF-8 Vfs paths (leading `/`).
pub struct VfsPathFs<'a>(pub &'a Vfs);

impl VfsPathFs<'_> {
    fn with<T>(&self, p: &str, f: impl FnOnce(&Vfs, Handle) -> Result<T, Errno>) -> io::Result<T> {
        let (h, _) = self.0.open(p.as_bytes()).map_err(errno_to_io)?;
        let r = f(self.0, h);
        self.0.close(h);
        r.map_err(errno_to_io)
    }
}

impl PathFs for VfsPathFs<'_> {
    fn create_dir(&self, p: &str) -> io::Result<()> {
        let (h, _) = self
            .0
            .create(p.as_bytes(), FileType::Dir, 0o755)
            .map_err(errno_to_io)?;
        self.0.close(h);
        Ok(())
    }

    fn write_file(&self, p: &str, off: u64, data: &[u8]) -> io::Result<()> {
        let h = match self.0.open(p.as_bytes()) {
            Ok((h, _)) => h,
            Err(Errno::ENOENT) => {
                self.0
                    .create(p.as_bytes(), FileType::Reg, 0o644)
                    .map_err(errno_to_io)?
                    .0
            }
            Err(e) => return Err(errno_to_io(e)),
        };
        let mut done = 0;
        let r = loop {
            if done == data.len() {
                break Ok(());
            }
            match self.0.write(h, off + done as u64, &data[done..]) {
                Ok(0) => break Err(io::Error::from(ErrorKind::WriteZero)),
                Ok(n) => done += n,
                Err(e) => break Err(errno_to_io(e)),
            }
        };
        self.0.close(h);
        r
    }

    fn read_file(&self, p: &str, off: u64, len: usize) -> io::Result<Vec<u8>> {
        self.with(p, |v, h| {
            let mut buf = vec![0u8; len];
            let mut done = 0;
            while done < len {
                let n = v.read(h, off + done as u64, &mut buf[done..])?;
                if n == 0 {
                    break;
                }
                done += n;
            }
            buf.truncate(done);
            Ok(buf)
        })
    }

    fn set_len(&self, p: &str, len: u64) -> io::Result<()> {
        self.with(p, |v, h| v.set_len(h, len))
    }

    fn remove_file(&self, p: &str) -> io::Result<()> {
        self.0.unlink(p.as_bytes()).map_err(errno_to_io)
    }

    fn remove_dir(&self, p: &str) -> io::Result<()> {
        self.0.rmdir(p.as_bytes()).map_err(errno_to_io)
    }

    fn rename(&self, from: &str, to: &str) -> io::Result<()> {
        self.0
            .rename(from.as_bytes(), to.as_bytes(), true)
            .map_err(errno_to_io)
    }

    fn list_dir(&self, p: &str) -> io::Result<Vec<String>> {
        let names = self.with(p, |v, h| -> Result<Vec<Vec<u8>>, Errno> {
            let mut out = Vec::new();
            let mut cookie = 0;
            loop {
                let page = v.read_dir(h, cookie, 256)?;
                let Some(last) = page.last() else {
                    return Ok(out);
                };
                cookie = last.cookie;
                out.extend(page.into_iter().map(|e| e.name));
            }
        })?;
        names
            .into_iter()
            .map(|n| {
                String::from_utf8(n)
                    .map_err(|_| io::Error::new(ErrorKind::InvalidData, "non-UTF-8 name"))
            })
            .collect()
    }

    fn is_dir(&self, p: &str) -> io::Result<bool> {
        self.with(p, |v, h| v.stat(h))
            .map(|s| s.ftype == FileType::Dir)
    }

    fn len(&self, p: &str) -> io::Result<u64> {
        self.with(p, |v, h| v.stat(h)).map(|s| s.size)
    }
}
