// SPDX-License-Identifier: GPL-2.0-only
//! Smoke tests for the runtime WinFsp loader. They need a Windows host with
//! WinFsp 2.x installed (the test VM, see docs/vm.md), so the whole file is
//! compiled only for Windows.
#![cfg(windows)]

use std::sync::Mutex;

use winuxfs_win::{winfsp_version, WinFsp, WinFspError, WINFSP_DIR_ENV};

/// The loader reads `WINUXFS_WINFSP_DIR`; tests in this binary run on
/// parallel threads, so every test holds this lock while it depends on the
/// environment.
static ENV_LOCK: Mutex<()> = Mutex::new(());

fn env_guard() -> std::sync::MutexGuard<'static, ()> {
    ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

#[test]
fn load_finds_winfsp() {
    let _g = env_guard();
    std::env::remove_var(WINFSP_DIR_ENV);
    let fsp = WinFsp::load();
    assert!(fsp.is_ok(), "WinFsp::load failed: {:?}", fsp.err());
    let fsp = fsp.unwrap();
    assert!(fsp.dll_path().ends_with("bin\\winfsp-x64.dll"));
}

#[test]
fn version_is_2x() {
    let _g = env_guard();
    std::env::remove_var(WINFSP_DIR_ENV);
    let (major, minor) = winfsp_version().unwrap();
    eprintln!("WinFsp DLL file version {major}.{minor}");
    assert_eq!(major, 2);
}

#[test]
fn missing_install_dir_reports_not_installed() {
    let _g = env_guard();
    std::env::set_var(WINFSP_DIR_ENV, r"C:\winuxfs-no-such-dir\WinFsp");
    let res = WinFsp::load();
    let ver = winfsp_version();
    std::env::remove_var(WINFSP_DIR_ENV);
    assert!(
        matches!(res, Err(WinFspError::NotInstalled)),
        "{:?}",
        res.err()
    );
    assert!(matches!(ver, Err(WinFspError::NotInstalled)), "{ver:?}");
}

#[test]
fn relative_override_is_rejected() {
    let _g = env_guard();
    for dir in ["WinFsp", r"\Program Files (x86)\WinFsp", r"C:WinFsp"] {
        std::env::set_var(WINFSP_DIR_ENV, dir);
        let res = WinFsp::load();
        std::env::remove_var(WINFSP_DIR_ENV);
        assert!(
            matches!(res, Err(WinFspError::InvalidOverride)),
            "{dir}: {:?}",
            res.err()
        );
    }
}

#[test]
fn add_dir_info_wrapper_writes_end_marker() {
    let _g = env_guard();
    std::env::remove_var(WINFSP_DIR_ENV);
    let fsp = WinFsp::load().unwrap();
    let mut buf = [0xAAu8; 16];
    let mut done = 0u32;
    // SAFETY: a null entry asks for the end marker; buf has 16 bytes.
    let ok = unsafe {
        fsp.add_dir_info(
            std::ptr::null_mut(),
            buf.as_mut_ptr().cast(),
            buf.len() as u32,
            &mut done,
        )
    };
    assert!(ok);
    assert_eq!(done, 2);
    assert_eq!(&buf[..3], &[0, 0, 0xAA]);
    // A clone keeps the DLL loaded after the original is dropped.
    let clone = fsp.clone();
    drop(fsp);
    let mut done = 0u32;
    // SAFETY: as above.
    assert!(unsafe {
        clone.add_dir_info(
            std::ptr::null_mut(),
            buf.as_mut_ptr().cast(),
            buf.len() as u32,
            &mut done,
        )
    });
}
