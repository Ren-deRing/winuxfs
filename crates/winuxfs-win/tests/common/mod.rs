// SPDX-License-Identifier: GPL-2.0-only
//! Shared helpers for the Windows end-to-end tests (`e2e_write`,
//! `e2e_conformance`): a memfs instance wrapped in a Vfs and mounted on
//! [`DRIVE`]. Needs WinFsp 2.x and `winuxfs_backend_memfs.dll` next to the
//! test executable (or at `WINUXFS_MEMFS_DLL`); see `e2e_read.rs`.
#![allow(dead_code)]

use std::os::windows::ffi::OsStrExt;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};

use winuxfs_core::plugin::{load_plugin, Devices, FileType};
use winuxfs_core::vfs::{Vfs, VfsOptions};
use winuxfs_win::{mount_vfs, MountOptions, MountedFs, WinFsp};

/// Drive letter the tests mount on (`Z:` is the VM's Samba share).
pub const DRIVE: &str = "Y:";
pub const LABEL: &str = "winuxfs-test";

static LOCK: Mutex<()> = Mutex::new(());

/// Serialises the tests of one process: they share the drive letter.
pub fn lock() -> MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

pub fn memfs_dll() -> PathBuf {
    if let Some(p) = std::env::var_os("WINUXFS_MEMFS_DLL") {
        return PathBuf::from(p);
    }
    let exe = std::env::current_exe().unwrap();
    exe.parent().unwrap().join("winuxfs_backend_memfs.dll")
}

/// `Y:\<rel>`.
pub fn root(rel: &str) -> PathBuf {
    PathBuf::from(format!("{DRIVE}\\{rel}"))
}

/// NUL-terminated UTF-16.
pub fn wide(s: &str) -> Vec<u16> {
    std::ffi::OsStr::new(s)
        .encode_wide()
        .chain(Some(0))
        .collect()
}

pub fn mkfile(v: &Vfs, p: &[u8], data: &[u8], mode: u32) {
    let (h, _) = v.create(p, FileType::Reg, mode).unwrap();
    assert_eq!(v.write(h, 0, data).unwrap(), data.len());
    v.close(h);
}

pub fn mkdir(v: &Vfs, p: &[u8]) {
    let (h, _) = v.create(p, FileType::Dir, 0o755).unwrap();
    v.close(h);
}

/// Reads a whole file through the Vfs (bypassing Windows).
pub fn vfs_read(v: &Vfs, p: &[u8]) -> Result<Vec<u8>, winuxfs_core::Errno> {
    let (h, st) = v.open(p)?;
    let mut buf = vec![0u8; st.size as usize];
    let mut done = 0;
    while done < buf.len() {
        match v.read(h, done as u64, &mut buf[done..]) {
            Ok(0) => break,
            Ok(n) => done += n,
            Err(e) => {
                v.close(h);
                return Err(e);
            }
        }
    }
    v.close(h);
    buf.truncate(done);
    Ok(buf)
}

/// Mount configuration.
#[derive(Clone, Copy, Default)]
pub struct Setup {
    pub case_insensitive: bool,
    pub read_only: bool,
}

/// A mounted memfs. Field order matters: the mount goes before the lock.
pub struct Mounted {
    pub fs: Option<MountedFs>,
    pub vfs: Arc<Vfs>,
    _guard: MutexGuard<'static, ()>,
}

/// A fresh memfs, populated by `fill` through a writable Vfs, then mounted
/// with `s`.
pub fn mount_with(s: Setup, fill: impl FnOnce(&Vfs)) -> Mounted {
    let guard = lock();
    let backend = load_plugin(&memfs_dll()).expect("load memfs plugin");
    let devs = Devices {
        data: None,
        log: None,
        rt: None,
    };
    let mount = Arc::new(backend.mount(devs, "").expect("mount memfs"));
    {
        let w = Vfs::new(mount.clone(), VfsOptions::default());
        fill(&w);
    }
    let vfs = Arc::new(Vfs::new(
        mount,
        VfsOptions {
            case_insensitive: s.case_insensitive,
            read_only: s.read_only,
        },
    ));
    let fsp = WinFsp::load().expect("load WinFsp");
    let opts = MountOptions {
        volume_label: LABEL.to_string(),
        read_only: s.read_only,
        case_sensitive: !s.case_insensitive,
        allow_users: false,
    };
    let fs = mount_vfs(&fsp, vfs.clone(), DRIVE, &opts).expect("mount_vfs");
    Mounted {
        fs: Some(fs),
        vfs,
        _guard: guard,
    }
}

/// An empty, writable, case-sensitive mount.
pub fn mount_empty() -> Mounted {
    mount_with(Setup::default(), |_| {})
}

/// Sorted names in a directory.
pub fn names(dir: &std::path::Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().into_string().unwrap())
        .collect();
    v.sort();
    v
}
