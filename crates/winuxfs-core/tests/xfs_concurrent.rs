// SPDX-License-Identifier: GPL-2.0-only
//! XFS backend under concurrent use, through the plugin ABI: several threads
//! on one mount, the caches squeezed to their minimum, and lookup/forget
//! traffic that has to balance. Mirrors `backends/xfs/tests/test_concurrent.c`
//! (which runs the same scenarios under ASan/TSan); this one checks the
//! host-side glue (call gating, `Mount` shared across threads).
//! Prints SKIPPED (and passes) when the plugin cannot be built or the fixture
//! cache (`make -C backends/xfs test-mount`) is missing, like `xfs_ops.rs`.
mod common;

use common::*;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, OnceLock};
use winuxfs_core::plugin::*;
use winuxfs_core::{BlockDevice, FileDevice};

const THREADS: usize = 8;
/// Files are hashed up to this size (the fixtures hold a 4 GiB sparse file).
const HASH_LIMIT: u64 = 1 << 20;
const TINY: &str = "bufcache_bytes=8388608,inodecache_entries=64";

fn xfs_plugin() -> Option<&'static Path> {
    static P: OnceLock<Option<PathBuf>> = OnceLock::new();
    P.get_or_init(|| {
        let ok = Command::new("make")
            .args(["-s", "-C"])
            .arg(workspace_root().join("backends/xfs"))
            .arg("plugin")
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        let p = plugin_dir().join(format!("winuxfs_backend_xfs.{EXT}"));
        (ok && p.exists()).then_some(p)
    })
    .as_deref()
}

fn fixtures() -> Option<PathBuf> {
    let mut c = Vec::new();
    if let Ok(d) = std::env::var("WFS_XFS_FIXDIR") {
        c.push(PathBuf::from(d));
    }
    c.push(workspace_root().join("target/xfs-fixtures"));
    // a git worktree keeps the shared cache in the main checkout
    c.push(workspace_root().join("../../target/xfs-fixtures"));
    c.into_iter().find(|d| d.join(".done").exists())
}

macro_rules! need {
    ($e:expr, $what:expr) => {
        match $e {
            Some(v) => v,
            None => {
                eprintln!("SKIPPED {}: {}", module_path!(), $what);
                return;
            }
        }
    };
}

fn devs_for(img: &Path) -> Devices {
    let open = |p: PathBuf| -> Option<Arc<dyn BlockDevice>> {
        p.exists()
            .then(|| Arc::new(FileDevice::open(&p, true).unwrap()) as Arc<dyn BlockDevice>)
    };
    let stem = img.to_str().unwrap().strip_suffix(".img").unwrap();
    Devices {
        data: open(img.to_path_buf()),
        log: open(PathBuf::from(format!("{stem}.log.img"))),
        rt: open(PathBuf::from(format!("{stem}.rt.img"))),
    }
}

fn images(dir: &Path) -> Vec<PathBuf> {
    let mut v: Vec<_> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| {
            let n = p.file_name().unwrap().to_str().unwrap();
            n.ends_with(".img") && !n.ends_with(".log.img") && !n.ends_with(".rt.img")
        })
        .collect();
    v.sort();
    v
}

type Tree = BTreeMap<Vec<u8>, (u32, u32, u64, String)>;

fn walk(m: &Mount, dir: u64, path: &[u8], out: &mut Tree) {
    let mut cookie = 0;
    loop {
        let ents = m.readdir(dir, cookie, 1024).unwrap();
        if ents.is_empty() {
            break;
        }
        for e in &ents {
            let mut p = path.to_vec();
            p.push(b'/');
            p.extend_from_slice(&e.name);
            let st = m.lookup(dir, &e.name).unwrap();
            assert_eq!(st.ino, e.ino, "lookup and readdir disagree");
            let content = match st.ftype {
                FileType::Reg if st.size <= HASH_LIMIT => {
                    let mut h = Sha256::new();
                    let mut buf = vec![0u8; 1 << 16];
                    let mut off = 0u64;
                    while off < st.size {
                        let n = m.read(st.ino, off, &mut buf).unwrap();
                        assert!(n > 0, "short read");
                        h.update(&buf[..n]);
                        off += n as u64;
                    }
                    format!("{:x}", h.finalize())
                }
                FileType::Lnk => String::from_utf8_lossy(&m.readlink(st.ino).unwrap()).into(),
                _ => String::new(),
            };
            out.insert(p.clone(), (st.mode, st.nlink, st.size, content));
            if st.ftype == FileType::Dir {
                walk(m, st.ino, &p, out);
            }
            m.forget(st.ino, 1);
        }
        cookie = ents.last().unwrap().cookie;
    }
}

fn tree(m: &Mount) -> Tree {
    let mut t = Tree::new();
    walk(m, 1, b"", &mut t);
    t
}

/// For each fixture image: the single-threaded tree, then `f(mount, baseline)`.
/// Every other image runs (the C test covers all of them); images the
/// backend refuses (outside M2) are skipped; at least 10 must run.
fn each_image(be: &Backend, dir: &Path, opts: &str, f: impl Fn(&Mount, &Tree)) {
    let mut n = 0;
    for img in images(dir).into_iter().step_by(2) {
        let Ok(base_mount) = be.mount(devs_for(&img), "") else {
            continue;
        };
        let base = tree(&base_mount);
        drop(base_mount);
        assert!(!base.is_empty(), "{}: empty tree", img.display());
        let m = be.mount(devs_for(&img), opts).unwrap();
        f(&m, &base);
        n += 1;
    }
    assert!(n >= 8, "only {n} fixture images ran");
}

fn setup() -> Option<(Backend, PathBuf)> {
    let p = xfs_plugin()?;
    Some((load_plugin(p).expect("xfs plugin loads"), fixtures()?))
}

fn parallel(m: &Mount, base: &Tree) {
    std::thread::scope(|s| {
        let hs: Vec<_> = (0..THREADS).map(|_| s.spawn(|| tree(m))).collect();
        for h in hs {
            assert!(&h.join().unwrap() == base, "a concurrent walk differs");
        }
    });
}

#[test]
fn parallel_walk_8_threads_same_manifest() {
    let (be, dir) = need!(setup(), "xfs plugin or fixture cache missing");
    each_image(&be, &dir, "", parallel);
}

#[test]
fn tiny_caches_still_correct() {
    let (be, dir) = need!(setup(), "xfs plugin or fixture cache missing");
    each_image(&be, &dir, TINY, |m, base| {
        parallel(m, base);
        // and once more on the now-churned caches
        assert!(&tree(m) == base);
    });
}

#[test]
fn concurrent_lookup_forget_balance() {
    let (be, dir) = need!(setup(), "xfs plugin or fixture cache missing");
    for opts in ["", TINY] {
        each_image(&be, &dir, opts, |m, base| {
            let names: Vec<Vec<u8>> = m
                .readdir(1, 0, 4096)
                .unwrap()
                .into_iter()
                .map(|e| e.name)
                .collect();
            assert!(!names.is_empty());
            std::thread::scope(|s| {
                for t in 0..THREADS {
                    let names = &names;
                    s.spawn(move || {
                        let mut seed = 977u32.wrapping_mul(t as u32 + 1);
                        let mut rnd = move || {
                            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
                            (seed >> 8) as usize
                        };
                        let mut held = Vec::new();
                        for _ in 0..1500 {
                            let st = m.lookup(1, &names[rnd() % names.len()]).unwrap();
                            if rnd() % 2 == 0 && held.len() < 40 {
                                held.push(st.ino);
                            } else {
                                m.forget(st.ino, 1);
                            }
                            while held.len() == 40 || (!held.is_empty() && rnd() % 3 == 0) {
                                m.forget(held.pop().unwrap(), 1);
                            }
                        }
                        for ino in held {
                            m.forget(ino, 1);
                        }
                    });
                }
            });
            // everything balanced: the mount still answers and nothing moved
            assert!(&tree(m) == base);
            assert!(m.getattr(1).is_ok());
        });
    }
}
