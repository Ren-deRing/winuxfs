// SPDX-License-Identifier: GPL-2.0-only
//! Command-line tests that run on every host (the Windows mount path is
//! covered by crates/winuxfs-win/tests/e2e_cli.rs).

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

use assert_cmd::Command as Cli;

const EXT: &str = if cfg!(windows) { "dll" } else { "so" };

fn workspace_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

static BUILD: Mutex<()> = Mutex::new(());

/// Builds `backends/<dir>` into `target/cli-test-plugins/<dir>` and returns
/// that directory (it holds only that backend's plugins).
fn plugins(dir: &str) -> PathBuf {
    let _g = BUILD.lock().unwrap_or_else(|e| e.into_inner());
    let out = workspace_root().join("target/cli-test-plugins").join(dir);
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".into());
    let st = Command::new("make")
        .arg("-s")
        .arg("-C")
        .arg(workspace_root().join("backends").join(dir))
        .arg(format!("CC={cc}"))
        .arg(format!("EXT={EXT}"))
        .arg(format!("OUT={}", out.display()))
        .status()
        .expect("run make");
    assert!(st.success(), "make -C backends/{dir} failed");
    out
}

fn winuxfs() -> Cli {
    let mut c = Cli::cargo_bin("winuxfs").unwrap();
    // Keep the user's environment from adding plugins.
    c.env_remove("WINUXFS_PLUGIN_PATH");
    c
}

fn scratch(name: &str) -> PathBuf {
    let d = workspace_root()
        .join("target/cli-test-scratch")
        .join(format!("{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// 8 MiB image with an MBR holding two Linux partitions:
/// 1: LBA 2048, 2048 sectors; 2: LBA 4096, 4096 sectors.
fn mbr_image(path: &Path) {
    let mut img = vec![0u8; 8 << 20];
    for (slot, (start, len)) in [(2048u32, 2048u32), (4096, 4096)].iter().enumerate() {
        let e = 446 + slot * 16;
        img[e + 4] = 0x83;
        img[e + 8..e + 12].copy_from_slice(&start.to_le_bytes());
        img[e + 12..e + 16].copy_from_slice(&len.to_le_bytes());
    }
    img[510] = 0x55;
    img[511] = 0xAA;
    std::fs::write(path, img).unwrap();
}

fn stdout_of(o: &std::process::Output) -> String {
    String::from_utf8_lossy(&o.stdout).into_owned()
}

fn stderr_of(o: &std::process::Output) -> String {
    String::from_utf8_lossy(&o.stderr).into_owned()
}

#[test]
fn list_backends_shows_memfs() {
    let dir = plugins("memfs");
    let o = winuxfs()
        .arg("list-backends")
        .arg("--plugin-dir")
        .arg(&dir)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(0), "{}", stderr_of(&o));
    let out = stdout_of(&o);
    assert!(out.contains("memfs"), "{out}");
}

#[test]
fn list_backends_shows_xfs() {
    let dir = plugins("xfs");
    let o = winuxfs()
        .arg("list-backends")
        .arg("--plugin-dir")
        .arg(&dir)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(0), "{}", stderr_of(&o));
    let out = stdout_of(&o);
    assert!(out.contains("xfs"), "{out}");
}

#[test]
fn list_backends_reports_bad_plugin() {
    let bad = plugins("bad");
    let good = plugins("memfs");
    let o = winuxfs()
        .arg("list-backends")
        .arg("--plugin-dir")
        .arg(&bad)
        .arg("--plugin-dir")
        .arg(&good)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(0), "{}", stderr_of(&o));
    let out = stdout_of(&o);
    assert!(out.to_lowercase().contains("abi"), "{out}");
    let abi_line = out
        .lines()
        .find(|l| l.contains(&format!("winuxfs_backend_bad_abi.{EXT}")))
        .unwrap_or_else(|| panic!("no line for the bad-ABI plugin:\n{out}"));
    assert!(abi_line.to_lowercase().contains("abi"), "{abi_line}");
    for n in ["bad_noops", "bad_noentry"] {
        assert!(out.contains(&format!("winuxfs_backend_{n}.{EXT}")), "{out}");
    }
    // A broken plugin does not hide the good ones.
    assert!(out.contains("memfs"), "{out}");
}

#[test]
fn list_backends_without_plugins_says_so() {
    let empty = scratch("empty-plugins");
    let o = winuxfs()
        .arg("list-backends")
        .arg("--plugin-dir")
        .arg(&empty)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(0));
    assert!(stdout_of(&o).contains("no backends"), "{}", stdout_of(&o));
}

#[test]
fn list_backends_uses_plugin_path_env() {
    let dir = plugins("memfs");
    let o = winuxfs()
        .arg("list-backends")
        .env("WINUXFS_PLUGIN_PATH", &dir)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(0));
    assert!(stdout_of(&o).contains("memfs"), "{}", stdout_of(&o));
}

#[test]
fn list_partitions_matches_sgdisk() {
    let script = workspace_root().join("tools/test_part_xcheck.sh");
    let o = Command::new("bash")
        .arg(&script)
        .arg(env!("CARGO_BIN_EXE_winuxfs"))
        .output()
        .unwrap();
    let err = stderr_of(&o);
    match o.status.code() {
        Some(0) => {}
        // 77: sgdisk is missing; the script says which package provides it.
        // libtest hides this output, so tools/check.sh runs the script
        // itself and prints SKIPPED.
        Some(77) => eprintln!("list_partitions_matches_sgdisk skipped: {err}"),
        c => panic!("xcheck failed ({c:?}):\n{}\n{err}", stdout_of(&o)),
    }
}

#[test]
fn list_partitions_mbr_table() {
    let d = scratch("mbr");
    let img = d.join("mbr.img");
    mbr_image(&img);
    let o = winuxfs().arg("list-partitions").arg(&img).output().unwrap();
    assert_eq!(o.status.code(), Some(0), "{}", stderr_of(&o));
    let rows: Vec<Vec<String>> = stdout_of(&o)
        .lines()
        .map(|l| l.split_whitespace().map(str::to_owned).collect::<Vec<_>>())
        .filter(|f| f.first().is_some_and(|c| c.parse::<u32>().is_ok()))
        .collect();
    assert_eq!(rows.len(), 2, "{}", stdout_of(&o));
    assert_eq!(rows[0][..3], ["1", "1048576", "1048576"]);
    assert_eq!(rows[1][..3], ["2", "2097152", "2097152"]);
    assert!(rows[0][3..].join(" ").contains("0x83"));
}

#[test]
fn list_partitions_without_table() {
    let d = scratch("notable");
    let img = d.join("plain.img");
    std::fs::write(&img, vec![0u8; 1 << 20]).unwrap();
    let o = winuxfs().arg("list-partitions").arg(&img).output().unwrap();
    assert_eq!(o.status.code(), Some(0), "{}", stderr_of(&o));
    assert!(
        stdout_of(&o).contains("no partition table"),
        "{}",
        stdout_of(&o)
    );
}

#[test]
fn list_partitions_missing_device_errors() {
    let d = scratch("missing");
    let o = winuxfs()
        .arg("list-partitions")
        .arg(d.join("nope.img"))
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    assert!(stderr_of(&o).contains("nope.img"), "{}", stderr_of(&o));
}

#[test]
fn partitioned_image_without_flag_errors() {
    let plug = plugins("memfs");
    let d = scratch("needpart");
    let img = d.join("mbr.img");
    mbr_image(&img);
    let o = winuxfs()
        .args(["mount", "X:"])
        .arg(&img)
        .args(["--type", "memfs", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("--partition"), "{err}");
    assert!(err.contains("1048576") && err.contains("2097152"), "{err}");
}

#[test]
fn partition_number_not_in_table_errors() {
    let plug = plugins("memfs");
    let d = scratch("badpart");
    let img = d.join("mbr.img");
    mbr_image(&img);
    let o = winuxfs()
        .args(["mount", "X:"])
        .arg(&img)
        .args(["--type", "memfs", "--partition", "7", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains('7') && err.contains("2097152"), "{err}");
}

#[test]
fn unknown_type_errors() {
    let plug = plugins("memfs");
    let o = winuxfs()
        .args(["mount", "X:", "none", "--type", "nosuchfs", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("nosuchfs"), "{err}");
    assert!(err.contains("list-backends"), "{err}");
}

#[test]
fn auto_type_without_match_errors() {
    // memfs never claims a device in probe, so nothing matches.
    let plug = plugins("memfs");
    let o = winuxfs()
        .args(["mount", "X:", "none", "--type", "auto", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("--type"), "{err}");
}

#[test]
fn bad_arguments_exit_1() {
    let o = winuxfs().arg("mount").output().unwrap();
    assert_eq!(o.status.code(), Some(1));
    let o = winuxfs().arg("no-such-command").output().unwrap();
    assert_eq!(o.status.code(), Some(1));
    let o = winuxfs().arg("--help").output().unwrap();
    assert_eq!(o.status.code(), Some(0));
}

#[cfg(not(windows))]
#[test]
fn mount_is_windows_only() {
    // Valid arguments get past every check, then the host refuses.
    let plug = plugins("memfs");
    let o = winuxfs()
        .args(["mount", "X:", "none", "--type", "memfs", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    assert!(stderr_of(&o).contains("Windows"), "{}", stderr_of(&o));
    let o = winuxfs().args(["unmount", "X:"]).output().unwrap();
    assert_eq!(o.status.code(), Some(1));
    assert!(stderr_of(&o).contains("Windows"), "{}", stderr_of(&o));
}

/// A plugin directory holding only the named plugins built from
/// backends/testplug (`testplug` probes true when DATA starts with "TPLG").
fn testplug_dir(tag: &str, names: &[&str]) -> PathBuf {
    let built = plugins("testplug");
    let d = scratch(tag);
    for n in names {
        let f = format!("winuxfs_backend_{n}.{EXT}");
        std::fs::copy(built.join(&f), d.join(&f)).unwrap();
    }
    d
}

/// 1 MiB image whose first bytes are "TPLGabcdef".
fn tplg_image(dir: &Path) -> PathBuf {
    let p = dir.join("tplg.img");
    let mut img = vec![0u8; 1 << 20];
    img[..10].copy_from_slice(b"TPLGabcdef");
    std::fs::write(&p, img).unwrap();
    p
}

#[cfg(not(windows))]
#[test]
fn auto_type_picks_the_single_match() {
    let plug = testplug_dir("auto-one", &["testplug"]);
    let img = tplg_image(&scratch("auto-one-img"));
    let o = winuxfs()
        .args(["mount", "X:"])
        .arg(&img)
        .args(["--type", "auto", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    // Every check passed; only the host refuses to mount.
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("'testplug' would serve"), "{err}");
    assert!(err.contains("Windows"), "{err}");
}

#[test]
fn auto_type_with_several_matches_errors() {
    let plug = testplug_dir("auto-two", &["testplug", "testplug_st"]);
    let img = tplg_image(&scratch("auto-two-img"));
    let o = winuxfs()
        .args(["mount", "X:"])
        .arg(&img)
        .args(["--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("Several backends"), "{err}");
    assert!(
        err.contains("testplug,") || err.contains("testplug_st"),
        "{err}"
    );
    assert!(err.contains("--type"), "{err}");
}

#[test]
fn auto_type_with_no_recognised_device_errors() {
    let plug = testplug_dir("auto-zero", &["testplug", "testplug_st"]);
    let d = scratch("auto-zero-img");
    let img = d.join("zero.img");
    std::fs::write(&img, vec![0u8; 1 << 20]).unwrap();
    let o = winuxfs()
        .args(["mount", "X:"])
        .arg(&img)
        .args(["--type", "auto", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("No installed backend recognises"), "{err}");
}

#[cfg(not(windows))]
#[test]
fn ro_flag_reaches_the_mount_settings() {
    let plug = plugins("memfs");
    let o = winuxfs()
        .args([
            "mount",
            "X:",
            "none",
            "--type",
            "memfs",
            "--ro",
            "--plugin-dir",
        ])
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    assert!(stderr_of(&o).contains("read-only"), "{}", stderr_of(&o));
    let o = winuxfs()
        .args(["mount", "X:", "none", "--type", "memfs", "--plugin-dir"])
        .arg(&plug)
        .output()
        .unwrap();
    assert!(!stderr_of(&o).contains("read-only"), "{}", stderr_of(&o));
}

#[test]
fn log_device_same_as_data_errors() {
    let plug = plugins("memfs");
    let img = tplg_image(&scratch("samelog"));
    let o = winuxfs()
        .args(["mount", "X:"])
        .arg(&img)
        .args(["--type", "memfs", "--log"])
        .arg(&img)
        .arg("--plugin-dir")
        .arg(&plug)
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(1));
    let err = stderr_of(&o);
    assert!(err.contains("--log") && err.contains("same"), "{err}");
}
