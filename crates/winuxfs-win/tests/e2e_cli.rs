// SPDX-License-Identifier: GPL-2.0-only
//! End-to-end tests of the `winuxfs` command on Windows (the test VM,
//! docs/vm.md). They run `winuxfs.exe` as a child process with the memfs
//! plugin; both are looked up next to the test executable, or at
//! `WINUXFS_CLI` and in `WINUXFS_PLUGIN_DIR`:
//!
//! ```sh
//! make -C backends/memfs CC=x86_64-w64-mingw32-gcc EXT=dll OUT=<dir>
//! cargo build -p winuxfs-cli --target x86_64-pc-windows-gnu
//! cargo test -p winuxfs-win --target x86_64-pc-windows-gnu --no-run
//! # copy winuxfs.exe, winuxfs_backend_memfs.dll and e2e_cli-*.exe into one
//! # directory in the VM and run the test exe there
//! ```
//!
//! The tests mount on [`DRIVE`] and are serialised by a lock (they share the
//! drive letter, and sending Ctrl-C re-attaches this process's console).
#![cfg(windows)]

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Output, Stdio};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

/// Drive letter the tests mount on (`Y:` belongs to the adapter tests and
/// `Z:` to the VM's share).
const DRIVE: &str = "X:";
const MOUNT_TIMEOUT: Duration = Duration::from_secs(10);
const EXIT_TIMEOUT: Duration = Duration::from_secs(10);

/// `CREATE_NO_WINDOW`: the child gets its own (hidden) console, so a
/// Ctrl-C can be sent to it alone.
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const CTRL_C_EVENT: u32 = 0;
const ATTACH_PARENT_PROCESS: u32 = u32::MAX;

#[link(name = "kernel32")]
extern "system" {
    fn FreeConsole() -> i32;
    fn AttachConsole(pid: u32) -> i32;
    fn GenerateConsoleCtrlEvent(event: u32, group: u32) -> i32;
    fn SetConsoleCtrlHandler(
        handler: Option<unsafe extern "system" fn(u32) -> i32>,
        add: i32,
    ) -> i32;
    fn GetLastError() -> u32;
    fn GetStdHandle(n: u32) -> *mut core::ffi::c_void;
    fn SetStdHandle(n: u32, h: *mut core::ffi::c_void) -> i32;
}

static LOCK: Mutex<()> = Mutex::new(());

fn lock() -> MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn cli() -> PathBuf {
    std::env::var_os("WINUXFS_CLI")
        .map(PathBuf::from)
        .unwrap_or_else(|| exe_dir().join("winuxfs.exe"))
}

fn plugin_dir() -> PathBuf {
    std::env::var_os("WINUXFS_PLUGIN_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(exe_dir)
}

fn root(rel: &str) -> PathBuf {
    PathBuf::from(format!("{DRIVE}\\{rel}"))
}

fn drive_present() -> bool {
    Path::new(&format!("{DRIVE}\\")).exists()
}

fn winuxfs(args: &[&str]) -> Output {
    Command::new(cli())
        .args(args)
        .env_remove("WINUXFS_PLUGIN_PATH")
        .output()
        .expect("run winuxfs.exe")
}

/// A running `winuxfs mount`; killed on drop if a test failed midway.
struct Mount {
    child: Option<Child>,
}

impl Mount {
    fn start(extra: &[&str]) -> Mount {
        assert!(!drive_present(), "{DRIVE} is in use before the test");
        let child = Command::new(cli())
            .args(["mount", DRIVE, "none", "--type", "memfs", "--plugin-dir"])
            .arg(plugin_dir())
            .args(extra)
            .env_remove("WINUXFS_PLUGIN_PATH")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .creation_flags(CREATE_NO_WINDOW)
            .spawn()
            .expect("start winuxfs mount");
        let mut m = Mount { child: Some(child) };
        let t = Instant::now();
        while !drive_present() {
            if let Some(st) = m.child_mut().try_wait().unwrap() {
                let out = m.child.take().unwrap().wait_with_output().unwrap();
                panic!(
                    "winuxfs mount exited early ({st}): {}{}",
                    String::from_utf8_lossy(&out.stdout),
                    String::from_utf8_lossy(&out.stderr)
                );
            }
            assert!(
                t.elapsed() < MOUNT_TIMEOUT,
                "{DRIVE} did not appear in time"
            );
            std::thread::sleep(Duration::from_millis(100));
        }
        m
    }

    fn child_mut(&mut self) -> &mut Child {
        self.child.as_mut().unwrap()
    }

    fn pid(&self) -> u32 {
        self.child.as_ref().unwrap().id()
    }

    /// Waits up to `EXIT_TIMEOUT` for the child to exit; returns its status
    /// and output, or panics (after killing it) on timeout.
    fn wait_exit(mut self) -> (ExitStatus, String) {
        let t = Instant::now();
        loop {
            if self.child_mut().try_wait().unwrap().is_some() {
                break;
            }
            if t.elapsed() > EXIT_TIMEOUT {
                let _ = self.child_mut().kill();
                panic!("winuxfs mount did not exit within {EXIT_TIMEOUT:?}");
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        let out = self.child.take().unwrap().wait_with_output().unwrap();
        let text = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        (out.status, text)
    }
}

impl Drop for Mount {
    fn drop(&mut self) {
        if let Some(mut c) = self.child.take() {
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}

/// Sends a real Ctrl-C to the console of process `pid`: detach from our
/// console, attach to the child's, generate the event for everyone on it
/// (this process ignores it meanwhile), then go back. Attaching replaces
/// the standard handles, so they are saved and restored around it.
fn send_ctrl_c(pid: u32) {
    const STD_HANDLES: [u32; 3] = [-10i32 as u32, -11i32 as u32, -12i32 as u32];
    // SAFETY: plain Win32 console calls; the saved standard handles stay
    // owned by this process and are only put back.
    unsafe {
        let saved = STD_HANDLES.map(|n| GetStdHandle(n));
        FreeConsole();
        let attached = AttachConsole(pid);
        let attach_err = GetLastError();
        let (ok, err) = if attached != 0 {
            SetConsoleCtrlHandler(None, 1);
            let ok = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
            let err = GetLastError();
            // Give the event time to be delivered before we stop ignoring it.
            std::thread::sleep(Duration::from_millis(500));
            FreeConsole();
            SetConsoleCtrlHandler(None, 0);
            (ok, err)
        } else {
            (0, 0)
        };
        AttachConsole(ATTACH_PARENT_PROCESS);
        for (n, h) in STD_HANDLES.iter().zip(saved) {
            SetStdHandle(*n, h);
        }
        assert!(attached != 0, "AttachConsole({pid}) failed: {attach_err}");
        assert!(ok != 0, "GenerateConsoleCtrlEvent failed: {err}");
    }
}

fn wait_drive_gone() {
    let t = Instant::now();
    while drive_present() {
        assert!(t.elapsed() < MOUNT_TIMEOUT, "{DRIVE} still present");
        std::thread::sleep(Duration::from_millis(100));
    }
}

#[test]
fn mount_memfs_and_use() {
    let _g = lock();
    let m = Mount::start(&[]);
    let f = root("cli.txt");
    std::fs::write(&f, b"written through winuxfs mount").expect("write a file on the mount");
    assert_eq!(std::fs::read(&f).unwrap(), b"written through winuxfs mount");
    send_ctrl_c(m.pid());
    let (st, out) = m.wait_exit();
    assert_eq!(st.code(), Some(0), "{out}");
    wait_drive_gone();
}

#[test]
fn mount_memfs_list_and_ctrl_c() {
    // The read-only part of the above: works without the adapter write path.
    let _g = lock();
    let m = Mount::start(&[]);
    assert_eq!(std::fs::read_dir(root("")).unwrap().count(), 0);
    let o = Command::new("cmd")
        .args(["/c", "vol", DRIVE])
        .output()
        .unwrap();
    assert!(
        String::from_utf8_lossy(&o.stdout).contains("memfs"),
        "volume label: {}",
        String::from_utf8_lossy(&o.stdout)
    );
    send_ctrl_c(m.pid());
    let (st, out) = m.wait_exit();
    assert_eq!(st.code(), Some(0), "{out}");
    assert!(out.contains("Unmounted"), "{out}");
    wait_drive_gone();
}

#[test]
fn mount_on_used_letter_fails_cleanly() {
    let _g = lock();
    let before: Vec<_> = std::fs::read_dir("C:\\")
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect();
    let o = Command::new(cli())
        .args(["mount", "C:", "none", "--type", "memfs", "--plugin-dir"])
        .arg(plugin_dir())
        .output()
        .unwrap();
    let err = String::from_utf8_lossy(&o.stderr);
    assert_eq!(o.status.code(), Some(2), "{err}");
    assert!(err.contains("C:") && err.contains("in use"), "{err}");
    let after: Vec<_> = std::fs::read_dir("C:\\")
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect();
    assert_eq!(before, after);
    assert!(Path::new("C:\\Windows\\System32").is_dir());
}

#[test]
fn ctrl_c_with_open_handle() {
    let _g = lock();
    let m = Mount::start(&[]);
    // A handle on the volume's root directory stays open across Ctrl-C.
    use std::os::windows::fs::OpenOptionsExt;
    const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
    let held = std::fs::OpenOptions::new()
        .read(true)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
        .open(root(""))
        .expect("open the root directory");
    send_ctrl_c(m.pid());
    let t = Instant::now();
    let (st, out) = m.wait_exit();
    assert!(t.elapsed() < EXIT_TIMEOUT);
    assert_eq!(st.code(), Some(0), "{out}");
    drop(held);
    wait_drive_gone();
}

#[test]
fn unmount_command_stops_mount() {
    let _g = lock();
    let m = Mount::start(&[]);
    let o = winuxfs(&["unmount", DRIVE]);
    assert_eq!(
        o.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&o.stderr)
    );
    let (st, out) = m.wait_exit();
    assert_eq!(st.code(), Some(0), "{out}");
    wait_drive_gone();
    // Nothing mounted any more.
    let o = winuxfs(&["unmount", DRIVE]);
    assert_eq!(o.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&o.stderr).contains("Nothing is mounted"));
}

#[test]
fn mount_read_only_refuses_writes() {
    let _g = lock();
    let m = Mount::start(&["--ro"]);
    assert!(std::fs::write(root("ro.txt"), b"x").is_err());
    send_ctrl_c(m.pid());
    let (st, out) = m.wait_exit();
    assert_eq!(st.code(), Some(0), "{out}");
    assert!(out.contains("read-only"), "{out}");
    wait_drive_gone();
}

#[test]
fn mount_requires_admin_message() {
    // An administrator can open every disk, so deny access to a file
    // instead: the CLI must map "access denied" to exit code 3 with advice.
    let _g = lock();
    let dir = std::env::temp_dir().join("winuxfs-e2e-cli");
    let _ = std::fs::create_dir_all(&dir);
    let f = dir.join("locked.img");
    let fs = f.to_str().unwrap();
    let _ = Command::new("icacls")
        .args([fs, "/remove:d", "*S-1-1-0"])
        .output();
    std::fs::write(&f, vec![0u8; 4096]).unwrap();
    let st = Command::new("icacls")
        .args([fs, "/deny", "*S-1-1-0:(R)"])
        .output()
        .unwrap();
    assert!(st.status.success(), "icacls /deny failed");
    let o = winuxfs(&["list-partitions", fs]);
    let _ = Command::new("icacls")
        .args([fs, "/remove:d", "*S-1-1-0"])
        .output();
    let _ = std::fs::remove_file(&f);
    let err = String::from_utf8_lossy(&o.stderr);
    assert_eq!(o.status.code(), Some(3), "{err}");
    assert!(
        err.contains("denied") && err.contains("administrator"),
        "{err}"
    );
}
