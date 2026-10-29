//! XFS backend through the host: mounts real mkfs.xfs images from the shared
//! fixture cache (`tools/gen-fixtures.sh`, built by `make -C backends/xfs
//! test-mount`) and checks the whole tree against the generator's manifest.
//! Tests print SKIPPED (and pass) when the plugin cannot be built or the
//! fixtures / tools are missing, so a bare checkout stays green.
mod common;

use common::*;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, OnceLock};
use winuxfs_core::plugin::*;
use winuxfs_core::{BlockDevice, Errno, FileDevice};

/// Files larger than this are only size-checked unless `WFS_XFS_FULL=1`.
const HASH_LIMIT: u64 = 64 << 20;

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

fn backend() -> Option<Backend> {
    let p = xfs_plugin()?;
    Some(load_plugin(p).expect("xfs plugin loads"))
}

fn devs_for(img: &Path) -> Devices {
    let open = |p: PathBuf| -> Option<Arc<dyn BlockDevice>> {
        p.exists()
            .then(|| Arc::new(FileDevice::open(&p, true).unwrap()) as Arc<dyn BlockDevice>)
    };
    let stem = img
        .to_str()
        .unwrap()
        .strip_suffix(".img")
        .unwrap()
        .to_string();
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

/// The manifest writer decodes names with Python's surrogateescape, so a
/// non-UTF-8 byte 0xXX appears as a lone `\udcXX` escape that JSON parsers
/// reject. Map those bytes (and, for the walk, invalid bytes) to the private
/// use characters U+F780..U+F7FF so both sides produce the same key.
fn fix_surrogates(text: &str) -> String {
    let b = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 5 < b.len() && b[i + 1] == b'u' {
            let hex = std::str::from_utf8(&b[i + 2..i + 6]).unwrap_or("");
            if let Ok(v) = u32::from_str_radix(hex, 16) {
                let prev_high = i >= 6
                    && &b[i - 6..i - 4] == b"\\u"
                    && u32::from_str_radix(std::str::from_utf8(&b[i - 4..i]).unwrap_or(""), 16)
                        .map(|h| (0xd800..0xdc00).contains(&h))
                        .unwrap_or(false);
                if (0xdc80..=0xdcff).contains(&v) && !prev_high {
                    out.push_str(&format!("\\u{:04x}", 0xf700 + (v - 0xdc00)));
                    i += 6;
                    continue;
                }
            }
        }
        // copy one UTF-8 character
        let ch = text[i..].chars().next().unwrap();
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

/// Path component as a manifest key (invalid UTF-8 bytes -> U+F7XX).
fn name_key(name: &[u8]) -> String {
    let mut out = String::new();
    let mut rest = name;
    while !rest.is_empty() {
        match std::str::from_utf8(rest) {
            Ok(s) => {
                out.push_str(s);
                break;
            }
            Err(e) => {
                let (ok, bad) = rest.split_at(e.valid_up_to());
                out.push_str(std::str::from_utf8(ok).unwrap());
                out.push(char::from_u32(0xf700 + bad[0] as u32).unwrap());
                rest = &bad[1..];
            }
        }
    }
    out
}

/// One manifest entry as the generator writes it (`tools/xfs_manifest.py`).
#[derive(Debug, PartialEq)]
struct Ent {
    kind: &'static str,
    mode: u32,
    nlink: u64,
    size: Option<u64>,
    sha: Option<String>,
    target: Option<Vec<u8>>,
}

fn read_all(m: &Mount, ino: u64, size: u64) -> Option<String> {
    if size > HASH_LIMIT && std::env::var_os("WFS_XFS_FULL").is_none() {
        return None;
    }
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut off = 0u64;
    while off < size {
        let n = m.read(ino, off, &mut buf).unwrap();
        assert!(n > 0, "short read at {off} of {size}");
        h.update(&buf[..n]);
        off += n as u64;
    }
    Some(format!("{:x}", h.finalize()))
}

fn walk(m: &Mount, dir: u64, path: &str, out: &mut BTreeMap<String, Ent>) {
    let mut cookie = 0;
    loop {
        let ents = m.readdir(dir, cookie, 64).unwrap();
        if ents.is_empty() {
            break;
        }
        for e in &ents {
            let name = name_key(&e.name);
            let p = if path == "/" {
                format!("/{name}")
            } else {
                format!("{path}/{name}")
            };
            let st = m.lookup(dir, &e.name).unwrap();
            assert_eq!(st.ino, e.ino, "{p}: lookup and readdir disagree");
            let (kind, size, sha, target) = match st.ftype {
                FileType::Dir => ("dir", None, None, None),
                FileType::Reg => ("file", Some(st.size), read_all(m, st.ino, st.size), None),
                FileType::Lnk => ("symlink", None, None, Some(m.readlink(st.ino).unwrap())),
                FileType::Fifo => ("fifo", None, None, None),
                other => panic!("{p}: unexpected type {other:?}"),
            };
            out.insert(
                p.clone(),
                Ent {
                    kind,
                    mode: st.mode & 0o7777,
                    nlink: st.nlink as u64,
                    size,
                    sha,
                    target,
                },
            );
            if st.ftype == FileType::Dir {
                walk(m, st.ino, &p, out);
            }
            m.forget(st.ino, 1);
        }
        cookie = ents.last().unwrap().cookie;
    }
}

fn compare(img: &Path, got: &BTreeMap<String, Ent>) {
    let mf = img.to_str().unwrap().replace(".img", ".manifest.json");
    let text = std::fs::read_to_string(&mf).unwrap_or_else(|e| panic!("{mf}: {e}"));
    let want: serde_json::Value = serde_json::from_str(&fix_surrogates(&text)).unwrap();
    let ents = want["entries"].as_object().unwrap();
    let mut bad = Vec::new();
    for (p, w) in ents {
        if p == "/" {
            continue;
        }
        let Some(g) = got.get(p) else {
            bad.push(format!("missing {p}"));
            continue;
        };
        let kind = w["kind"].as_str().unwrap();
        if kind != g.kind {
            bad.push(format!("{p}: kind {} want {kind}", g.kind));
            continue;
        }
        if w["mode"].as_u64().unwrap() as u32 != g.mode && kind != "symlink" {
            bad.push(format!(
                "{p}: mode {:o} want {:o}",
                g.mode,
                w["mode"].as_u64().unwrap()
            ));
        }
        if let (Some(ws), Some(gs)) = (w["size"].as_u64(), g.size) {
            if ws != gs {
                bad.push(format!("{p}: size {gs} want {ws}"));
            }
        }
        if let (Some(ws), Some(gs)) = (w["sha256"].as_str(), g.sha.as_deref()) {
            if ws != "skipped" && ws != gs {
                bad.push(format!("{p}: sha256 {gs} want {ws}"));
            }
        }
        if let Some(t) = w["target"].as_str() {
            if g.target.as_deref() != Some(t.as_bytes()) {
                bad.push(format!("{p}: target {:?} want {t}", g.target));
            }
        }
        if w.get("hardlink_of").is_none() {
            if let Some(n) = w["nlink"].as_u64() {
                if kind != "dir" && n != g.nlink {
                    bad.push(format!("{p}: nlink {} want {n}", g.nlink));
                }
            }
        }
    }
    for p in got.keys() {
        if !ents.contains_key(p) {
            bad.push(format!("extra {p}"));
        }
    }
    assert!(
        bad.is_empty(),
        "{}: {} differences, first: {:?}",
        img.display(),
        bad.len(),
        &bad[..bad.len().min(8)]
    );
}

#[test]
fn mount_walk_matches_manifest() {
    let be = need!(backend(), "xfs plugin not buildable");
    let dir = need!(
        fixtures(),
        "no fixture cache (run make -C backends/xfs test-mount)"
    );
    let mut n = 0;
    for img in images(&dir) {
        let m = match be.mount(devs_for(&img), "") {
            Ok(m) => m,
            // fixtures with features outside M2 (e.g. v4) are refused, not walked
            Err(e) => {
                eprintln!("note: {} not mounted ({e:?})", img.display());
                continue;
            }
        };
        let mut got = BTreeMap::new();
        walk(&m, 1, "/", &mut got);
        compare(&img, &got);
        n += 1;
    }
    assert!(n >= 10, "only {n} fixture images were walked");
}

#[test]
fn read_only_write_ops_fail() {
    let be = need!(backend(), "xfs plugin not buildable");
    let dir = need!(fixtures(), "no fixture cache");
    let img = images(&dir).into_iter().next().unwrap();
    let m = be.mount(devs_for(&img), "").unwrap();
    assert_eq!(m.write(1, 0, b"x"), Err(Errno::ENOSYS));
    assert_eq!(m.unlink(1, b"nothing"), Err(Errno::ENOSYS));
    assert!(be.caps() & 1 != 0, "read-only capability not declared");
}

#[test]
fn auto_probe_picks_xfs() {
    let be = need!(backend(), "xfs plugin not buildable");
    let dir = need!(fixtures(), "no fixture cache");
    let img = images(&dir).into_iter().next().unwrap();
    assert_eq!(be.probe(&devs_for(&img)), Ok(true));
    // not an XFS image: zeros
    let z = Devices {
        data: Some(Arc::new(winuxfs_core::MemDevice::new(1 << 20, 512)) as Arc<dyn BlockDevice>),
        log: None,
        rt: None,
    };
    assert_eq!(be.probe(&z), Ok(false));
}

#[test]
fn hostile_options_are_rejected() {
    let be = need!(backend(), "xfs plugin not buildable");
    let dir = need!(fixtures(), "no fixture cache");
    let img = images(&dir).into_iter().next().unwrap();
    assert_eq!(be.mount(devs_for(&img), "bogus").err(), Some(Errno::EINVAL));
    assert!(be
        .mount(
            devs_for(&img),
            "bufcache_bytes=2097152,inodecache_entries=64"
        )
        .is_ok());
}

/// A kernel-written dirty log is a root-only fixture (tools/gen-root-fixtures.sh).
#[test]
fn dirty_image_mount_fails_with_euclean_and_norecovery_mounts() {
    let be = need!(backend(), "xfs plugin not buildable");
    let zst = workspace_root().join("tests/xfs/root-fixtures/dirty-log.img.zst");
    if !zst.exists() {
        eprintln!(
            "SKIPPED {}: dirty-log.img.zst not in tests/xfs/root-fixtures",
            module_path!()
        );
        return;
    }
    let out = scratch_dir("xfs-dirty").join("dirty-log.img");
    let ok = Command::new("zstd")
        .args(["-d", "-q", "-f", "-o"])
        .arg(&out)
        .arg(&zst)
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if !ok {
        eprintln!("SKIPPED {}: zstd missing", module_path!());
        return;
    }
    assert_eq!(be.mount(devs_for(&out), "").err(), Some(Errno::EUCLEAN));
    let m = be
        .mount(devs_for(&out), "norecovery")
        .expect("norecovery mounts");
    assert!(m.getattr(1).is_ok());
}

#[test]
fn surrogate_fix_unit() {
    let f = fix_surrogates("{\"a\\udce9\":1,\"b\\ud83d\\udc4d\":2}");
    assert!(serde_json::from_str::<serde_json::Value>(&f).is_ok(), "{f}");
    assert_eq!(name_key(b"caf\xe9"), "caf\u{f7e9}");
    assert!(f.contains("\\uf7e9"), "{f}");
}
