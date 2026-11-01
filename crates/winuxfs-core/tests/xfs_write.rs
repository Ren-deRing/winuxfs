// SPDX-License-Identifier: GPL-2.0-only
//! XFS backend write path through the host ABI (M3a T10): a fresh mkfs.xfs
//! image is mounted read-write, changed with every operation, unmounted, and
//! judged by the host's `xfs_repair -n`. Prints SKIPPED (and passes) when the
//! plugin cannot be built or xfsprogs is missing.
mod common;

use common::*;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, OnceLock};
use winuxfs_abi as abi;
use winuxfs_core::plugin::*;
use winuxfs_core::{BlockDevice, Errno, FileDevice};

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

fn have(tool: &str) -> bool {
    Command::new(tool)
        .arg("-V")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok()
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

fn scratch(name: &str) -> PathBuf {
    let d = std::env::var_os("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| workspace_root().join("target/tmp"));
    std::fs::create_dir_all(&d).unwrap();
    d.join(format!("xfs_write_{}_{name}.img", std::process::id()))
}

fn mkfs(path: &Path) -> bool {
    let f = std::fs::File::create(path).unwrap();
    f.set_len(320 << 20).unwrap();
    Command::new("mkfs.xfs")
        .args([
            "-q",
            "-f",
            "-m",
            "crc=1,rmapbt=0,reflink=0,metadir=0",
            "-n",
            "parent=0",
        ])
        .arg(path)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn repair_clean(path: &Path) -> bool {
    Command::new("xfs_repair")
        .arg("-n")
        .arg(path)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn devs(path: &Path, read_only: bool) -> Devices {
    Devices {
        data: Some(Arc::new(FileDevice::open(path, read_only).unwrap()) as Arc<dyn BlockDevice>),
        log: None,
        rt: None,
    }
}

fn pattern(n: usize, seed: u8) -> Vec<u8> {
    (0..n)
        .map(|i| (i as u8).wrapping_mul(7).wrapping_add(seed))
        .collect()
}

#[test]
fn write_path_through_the_abi() {
    let plugin = need!(xfs_plugin(), "xfs plugin not buildable");
    need!(
        (have("mkfs.xfs") && have("xfs_repair")).then_some(()),
        "xfsprogs missing"
    );
    let be = load_plugin(plugin).expect("xfs plugin loads");
    assert_eq!(
        be.caps() & 1,
        0,
        "the xfs backend no longer declares read-only"
    );

    let img = scratch("rw");
    assert!(mkfs(&img));
    let data = pattern(300_000, 3);
    {
        let m = be.mount(devs(&img, false), "").unwrap();
        let d = m.create(1, b"d", FileType::Dir, 0o755, 1000, 100).unwrap();
        assert_eq!((d.nlink, d.uid, d.gid), (2, 1000, 100));
        let f = m
            .create(d.ino, b"f", FileType::Reg, 0o640, 1000, 100)
            .unwrap();
        assert_eq!(m.write(f.ino, 0, &data).unwrap(), data.len());
        assert_eq!(m.write(f.ino, 1 << 32, b"far").unwrap(), 3);
        let mut a = f.clone();
        a.size = 1000;
        m.setattr(f.ino, &a, abi::WFS_SET_SIZE).unwrap();
        let mut back = vec![0u8; 2000];
        assert_eq!(m.read(f.ino, 0, &mut back).unwrap(), 1000);
        assert_eq!(&back[..1000], &data[..1000]);
        let l = m.symlink(d.ino, b"lnk", b"../d/f", 1000, 100).unwrap();
        assert_eq!(m.readlink(l.ino).unwrap(), b"../d/f");
        m.link(f.ino, 1, b"hard").unwrap();
        assert_eq!(m.lookup(1, b"hard").unwrap().nlink, 2);
        m.rename(d.ino, b"f", 1, b"moved", 0).unwrap();
        assert_eq!(
            m.rename(1, b"moved", 1, b"hard", abi::WFS_RENAME_NOREPLACE),
            Err(Errno::EEXIST)
        );
        m.fsync(f.ino, false).unwrap();
        m.sync().unwrap();
        m.unlink(1, b"hard").unwrap();
        assert_eq!(m.rmdir(1, b"d"), Err(Errno::ENOTEMPTY));
        m.unlink(d.ino, b"lnk").unwrap();
        m.rmdir(1, b"d").unwrap();
        m.forget(f.ino, 3);
        m.forget(l.ino, 1);
        m.forget(d.ino, 1);
        m.unmount().unwrap();
    }
    assert!(
        repair_clean(&img),
        "xfs_repair -n failed on {}",
        img.display()
    );
    {
        let m = be.mount(devs(&img, false), "").unwrap();
        let f = m.lookup(1, b"moved").unwrap();
        assert_eq!(f.size, 1000);
        let mut back = vec![0u8; 1000];
        assert_eq!(m.read(f.ino, 0, &mut back).unwrap(), 1000);
        assert_eq!(back, &data[..1000]);
        m.forget(f.ino, 1);
        m.unmount().unwrap();
    }
    let _ = std::fs::remove_file(&img);
}

#[test]
fn read_only_mount_refuses_changes() {
    let plugin = need!(xfs_plugin(), "xfs plugin not buildable");
    need!(
        (have("mkfs.xfs") && have("xfs_repair")).then_some(()),
        "xfsprogs missing"
    );
    let be = load_plugin(plugin).expect("xfs plugin loads");
    let img = scratch("ro");
    assert!(mkfs(&img));
    {
        let m = be.mount(devs(&img, true), "").unwrap();
        assert_eq!(
            m.create(1, b"x", FileType::Reg, 0o644, 0, 0).map(|_| ()),
            Err(Errno::EROFS)
        );
        assert_eq!(m.write(1, 0, b"x"), Err(Errno::EROFS));
        assert_eq!(m.unlink(1, b"nothing"), Err(Errno::EROFS));
        m.unmount().unwrap();
    }
    assert!(repair_clean(&img));
    let _ = std::fs::remove_file(&img);
}
