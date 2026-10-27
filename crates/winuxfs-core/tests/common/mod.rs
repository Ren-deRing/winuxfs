//! Shared helpers for plugin-dependent tests: build C test plugins on demand.
#![allow(dead_code)]

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

pub const EXT: &str = if cfg!(windows) { "dll" } else { "so" };

pub fn workspace_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

/// Output directory for test-only plugins (`bad`, `testplug`).
pub fn test_plugin_dir() -> PathBuf {
    workspace_root().join("target/test-plugins")
}

static BUILD: Mutex<Vec<String>> = Mutex::new(Vec::new());

/// Runs `make -C backends/<backend_dir>` once per process, so `cargo test`
/// works from a clean checkout and rebuilds stale plugins (make's own
/// dependency tracking keeps this a no-op when up to date). If `make` is not
/// available but the plugin already exists, the existing build is used.
fn build(backend_dir: &str, out: &Path) {
    let mut done = BUILD.lock().unwrap_or_else(|e| e.into_inner());
    if done.iter().any(|d| d == backend_dir) {
        return;
    }
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".into());
    let status = Command::new("make")
        .arg("-s")
        .arg("-C")
        .arg(workspace_root().join("backends").join(backend_dir))
        .arg(format!("CC={cc}"))
        .arg(format!("EXT={EXT}"))
        .arg(format!("OUT={}", out.display()))
        .status();
    match status {
        Ok(s) if s.success() => {}
        Ok(s) => panic!("make -C backends/{backend_dir} failed: {s}"),
        Err(e) => eprintln!("warning: could not run make for backends/{backend_dir}: {e}"),
    }
    done.push(backend_dir.to_string());
}

/// Path to `target/test-plugins/winuxfs_backend_<name>.<ext>`, built from
/// `backends/<backend_dir>` if needed.
pub fn test_plugin(backend_dir: &str, name: &str) -> PathBuf {
    let dir = test_plugin_dir();
    build(backend_dir, &dir);
    let p = dir.join(format!("winuxfs_backend_{name}.{EXT}"));
    assert!(p.exists(), "plugin {} was not built", p.display());
    p
}

/// Output directory for shipped plugins (`memfs`, ...).
pub fn plugin_dir() -> PathBuf {
    workspace_root().join("target/plugins")
}

/// Path to `target/plugins/winuxfs_backend_<name>.<ext>`, built from
/// `backends/<backend_dir>` if needed.
pub fn plugin(backend_dir: &str, name: &str) -> PathBuf {
    let dir = plugin_dir();
    build(backend_dir, &dir);
    let p = dir.join(format!("winuxfs_backend_{name}.{EXT}"));
    assert!(p.exists(), "plugin {} was not built", p.display());
    p
}

/// A fresh, empty scratch directory under `target/test-plugins/scratch`.
pub fn scratch_dir(tag: &str) -> PathBuf {
    let d = test_plugin_dir()
        .join("scratch")
        .join(format!("{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Copies a plugin to a unique path so it loads as a separate library
/// instance (with its own C globals) in this process.
pub fn private_copy(src: &Path, tag: &str) -> PathBuf {
    let d = scratch_dir(tag);
    let p = d.join(src.file_name().unwrap());
    std::fs::copy(src, &p).unwrap();
    p
}
