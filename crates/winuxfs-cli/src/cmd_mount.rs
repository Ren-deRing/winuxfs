// SPDX-License-Identifier: GPL-2.0-only
//! `mount` and `unmount`.
//!
//! Every check that does not need Windows (plugins, backend name, devices,
//! partitions, `--type auto` probing) runs on every host, so mistakes are
//! reported the same way everywhere; only serving the file system through
//! WinFsp is Windows-only. Off Windows the devices are always opened
//! read-only and the backend is never mounted.
//!
//! On Windows the mount runs in the foreground until Ctrl-C (or console
//! close) or `winuxfs unmount`, then tears down in order: mount point and
//! dispatcher (`MountedFs` drop, which also closes handles Windows still
//! held), Vfs and backend `Mount`, then the devices (a physical disk goes
//! back online, a VHD is detached).

use std::path::PathBuf;
use std::sync::Arc;

use clap::Args;
use winuxfs_abi as abi;
use winuxfs_core::plugin::{discover, Backend, Devices};
use winuxfs_core::BlockDevice;

use crate::common::{open_device, plugin_dirs, select_partition, CliError, Opened};

/// Arguments of `winuxfs mount`.
#[derive(Args, Debug)]
pub struct MountArgs {
    /// Drive letter (`X:`) or a directory that does not exist yet.
    pub mountpoint: String,
    /// Data device: `\\.\PhysicalDriveN`, `disk:N`, a .vhd/.vhdx file, an
    /// image file, or `none` for backends that need no device.
    pub device: String,
    /// Backend name (see `list-backends`), or `auto` to probe all of them.
    #[arg(long = "type", value_name = "NAME|auto", default_value = "auto")]
    pub fstype: String,
    /// Partition number (as shown by `list-partitions`); required when the
    /// device has a partition table.
    #[arg(long, value_name = "N")]
    pub partition: Option<u32>,
    /// External log device.
    #[arg(long, value_name = "DEV")]
    pub log: Option<String>,
    /// Realtime device.
    #[arg(long, value_name = "DEV")]
    pub rt: Option<String>,
    /// Mount read-only: devices are opened read-only and Windows sees a
    /// read-only volume.
    #[arg(long)]
    pub ro: bool,
    /// Match file names without regard to case.
    #[arg(long)]
    pub case_insensitive: bool,
    /// Backend-specific mount options.
    #[arg(long, value_name = "STR", default_value = "")]
    pub options: String,
    /// Plugin directory (repeatable); replaces the default directories.
    #[arg(long = "plugin-dir", value_name = "DIR")]
    pub plugin_dir: Vec<PathBuf>,
}

/// Opened devices, kept alive until the very end of the mount.
struct OpenDevices {
    /// What the backend gets.
    devs: [Option<Arc<dyn BlockDevice>>; 3],
    /// The opened whole devices (dropped after `devs`).
    _owners: Vec<Opened>,
}

impl OpenDevices {
    fn devices(&self) -> Devices {
        Devices {
            data: self.devs[0].clone(),
            log: self.devs[1].clone(),
            rt: self.devs[2].clone(),
        }
    }
}

fn open_all(a: &MountArgs, read_only: bool) -> Result<OpenDevices, CliError> {
    let mut owners = Vec::new();
    let data = if a.device.eq_ignore_ascii_case("none") {
        if a.partition.is_some() {
            return Err(CliError::usage(
                "--partition needs a device: give one instead of 'none'.",
            ));
        }
        None
    } else {
        let o = open_device(&a.device, read_only)?;
        let d = select_partition(&a.device, o.dev.clone(), a.partition)?;
        owners.push(o);
        Some(d)
    };
    let mut extra = |dev: &Option<String>| -> Result<Option<Arc<dyn BlockDevice>>, CliError> {
        let Some(name) = dev else { return Ok(None) };
        let o = open_device(name, read_only)?;
        let d = o.dev.clone();
        owners.push(o);
        Ok(Some(d))
    };
    let log = extra(&a.log)?;
    let rt = extra(&a.rt)?;
    Ok(OpenDevices {
        devs: [data, log, rt],
        _owners: owners,
    })
}

fn is_readonly_backend(b: &Backend) -> bool {
    b.caps() & abi::WFS_CAP_READONLY != 0
}

fn names(backends: &[&Backend]) -> String {
    backends
        .iter()
        .map(|b| b.name())
        .collect::<Vec<_>>()
        .join(", ")
}

/// The backend called `name` (exact match first, then ignoring ASCII case).
fn find_backend<'a>(backends: &'a [Backend], name: &str) -> Result<&'a Backend, CliError> {
    backends
        .iter()
        .find(|b| b.name() == name)
        .or_else(|| {
            backends
                .iter()
                .find(|b| b.name().eq_ignore_ascii_case(name))
        })
        .ok_or_else(|| {
            let all: Vec<&Backend> = backends.iter().collect();
            CliError::usage(format!(
                "Unknown file system type '{name}' (installed: {}): run 'winuxfs \
                 list-backends' to see the installed types.",
                if all.is_empty() {
                    "none".to_string()
                } else {
                    names(&all)
                }
            ))
        })
}

/// `--type auto`: the one backend whose probe accepts the devices.
fn probe_one<'a>(
    backends: &'a [Backend],
    devs: &OpenDevices,
    device: &str,
) -> Result<&'a Backend, CliError> {
    let hits: Vec<&Backend> = backends
        .iter()
        .filter(|b| b.probe(&devs.devices()).unwrap_or(false))
        .collect();
    match hits.len() {
        1 => Ok(hits[0]),
        0 => Err(CliError::usage(format!(
            "No installed backend recognises {device}: name the file system with \
             --type (see 'winuxfs list-backends')."
        ))),
        _ => Err(CliError::usage(format!(
            "Several backends recognise {device} ({}): choose one with --type.",
            names(&hits)
        ))),
    }
}

/// Runs `winuxfs mount`. On Windows it returns after the file system was
/// unmounted again.
pub fn mount(a: &MountArgs) -> Result<(), CliError> {
    // Claim the mount point first: if it is taken, nothing is opened.
    #[cfg(windows)]
    let claim = win::Claim::new(&a.mountpoint)?;

    let backends: Vec<Backend> = discover(&plugin_dirs(&a.plugin_dir))
        .into_iter()
        .filter_map(|(_, r)| r.ok())
        .collect();
    let auto = a.fstype.eq_ignore_ascii_case("auto");
    let named = if auto {
        None
    } else {
        Some(find_backend(&backends, &a.fstype)?)
    };
    // Off Windows nothing is mounted, so nothing may be written either.
    let open_ro = a.ro || named.is_some_and(is_readonly_backend) || cfg!(not(windows));
    let devs = open_all(a, open_ro)?;
    let backend = match named {
        Some(b) => b,
        None => probe_one(&backends, &devs, &a.device)?,
    };
    let read_only = a.ro || is_readonly_backend(backend);

    #[cfg(not(windows))]
    {
        let _ = (read_only, devs);
        Err(CliError::usage(format!(
            "Mounting needs Windows with WinFsp 2.x (the arguments are valid and \
             '{}' would serve {}): run winuxfs mount on Windows.",
            backend.name(),
            a.device
        )))
    }
    #[cfg(windows)]
    {
        win::serve(a, backend, devs, read_only, claim)
    }
}

/// Runs `winuxfs unmount`: asks the winuxfs process serving `mountpoint`
/// to unmount and waits until it has.
pub fn unmount(mountpoint: &str) -> Result<(), CliError> {
    #[cfg(not(windows))]
    {
        Err(CliError::usage(format!(
            "Unmounting needs Windows ({mountpoint} cannot be a winuxfs mount on this \
             system): run winuxfs unmount on Windows."
        )))
    }
    #[cfg(windows)]
    {
        win::unmount(mountpoint)
    }
}

#[cfg(windows)]
mod win {
    //! Mount point claim, Ctrl-C handling and the serve loop.
    //!
    //! A mount registers two named kernel objects in the session namespace,
    //! keyed by the normalised mount point: a manual-reset "stop" event that
    //! Ctrl-C and `winuxfs unmount` signal, and a "live" mutex the serving
    //! thread owns until teardown has finished (an exiting or killed process
    //! abandons it, which also wakes `unmount`).

    use std::ffi::c_void;
    use std::io::Write;
    use std::ptr::null;
    use std::sync::atomic::{AtomicPtr, Ordering};
    use std::sync::Arc;

    use windows_sys::core::BOOL;
    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, FALSE, HANDLE, TRUE, WAIT_ABANDONED,
        WAIT_OBJECT_0, WAIT_TIMEOUT,
    };
    use windows_sys::Win32::Storage::FileSystem::GetLogicalDrives;
    use windows_sys::Win32::System::Console::{
        SetConsoleCtrlHandler, CTRL_BREAK_EVENT, CTRL_C_EVENT,
    };
    use windows_sys::Win32::System::Threading::{
        CreateEventW, CreateMutexW, OpenEventW, OpenMutexW, ReleaseMutex, SetEvent,
        WaitForSingleObject, EVENT_MODIFY_STATE, INFINITE, SYNCHRONIZATION_SYNCHRONIZE,
    };
    use winuxfs_core::plugin::Backend;
    use winuxfs_core::vfs::{Vfs, VfsOptions};
    use winuxfs_win::{mount_vfs, MountOptions, WinFsp};

    use super::{MountArgs, OpenDevices};
    use crate::common::errno_text;
    use crate::common::CliError;

    /// How long `unmount` waits for the serving process to finish.
    const UNMOUNT_TIMEOUT_MS: u32 = 30_000;
    /// How long the console-close handler waits for teardown (Windows ends
    /// the process a few seconds after such an event anyway).
    const CLOSE_WAIT_MS: u32 = 10_000;

    /// The stop event of the mount in this process, for the Ctrl handler.
    static STOP: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
    /// Signalled when teardown has finished, for the Ctrl handler.
    static DONE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    /// The drive letter of `X:` or `X:\`, upper-cased.
    fn drive_letter(mp: &str) -> Option<char> {
        let b = mp.as_bytes();
        let ok = matches!(b.len(), 2 | 3)
            && b[0].is_ascii_alphabetic()
            && b[1] == b':'
            && (b.len() == 2 || b[2] == b'\\' || b[2] == b'/');
        ok.then(|| b[0].to_ascii_uppercase() as char)
    }

    /// The form handed to WinFsp (`X:` or the directory path).
    fn mount_point_arg(mp: &str) -> String {
        match drive_letter(mp) {
            Some(l) => format!("{l}:"),
            None => mp.to_string(),
        }
    }

    /// Object-name key: upper case, no backslashes (they are namespace
    /// separators in kernel object names).
    fn key(mp: &str) -> String {
        mount_point_arg(mp)
            .trim_end_matches(['\\', '/'])
            .to_uppercase()
            .replace('\\', "/")
    }

    fn object_name(kind: &str, mp: &str) -> Vec<u16> {
        wide(&format!("Local\\winuxfs-{kind}-{}", key(mp)))
    }

    struct OwnedHandle(HANDLE);

    impl Drop for OwnedHandle {
        fn drop(&mut self) {
            if !self.0.is_null() {
                // SAFETY: the handle came from a successful Create*/Open*
                // call and is closed once.
                unsafe { CloseHandle(self.0) };
            }
        }
    }

    /// The claimed mount point: the stop event and the owned live mutex.
    pub struct Claim {
        mount_point: String,
        stop: OwnedHandle,
        live: OwnedHandle,
        done: OwnedHandle,
    }

    impl Claim {
        /// Fails with exit code 2, before anything else is touched, if the
        /// mount point is in use.
        pub fn new(mp: &str) -> Result<Claim, CliError> {
            if mp.is_empty() || mp.contains('\0') {
                return Err(CliError::usage(
                    "The mount point is empty: give a drive letter such as X:.",
                ));
            }
            let in_use = || {
                CliError::mount(format!(
                    "{mp} is already in use: choose a free drive letter or unmount it first."
                ))
            };
            match drive_letter(mp) {
                Some(l) => {
                    // SAFETY: no preconditions.
                    let mask = unsafe { GetLogicalDrives() };
                    if mask & (1 << (l as u8 - b'A')) != 0 {
                        return Err(in_use());
                    }
                }
                None => {
                    if mp.len() == 2 && mp.ends_with(':') {
                        return Err(CliError::usage(format!(
                            "{mp} is not a drive letter: use a letter from A: to Z:."
                        )));
                    }
                    if std::path::Path::new(mp).exists() {
                        return Err(CliError::mount(format!(
                            "{mp} already exists: give a directory path that does not exist \
                             yet (WinFsp creates it)."
                        )));
                    }
                }
            }
            let stop_name = object_name("stop", mp);
            // SAFETY: valid NUL-terminated name; manual reset, not signalled.
            let stop = unsafe { CreateEventW(null(), TRUE, FALSE, stop_name.as_ptr()) };
            // SAFETY: no preconditions.
            let err = unsafe { GetLastError() };
            if stop.is_null() {
                return Err(CliError::mount(format!(
                    "Creating the stop event for {mp} failed (Win32 error {err}): try again."
                )));
            }
            let stop = OwnedHandle(stop);
            if err == ERROR_ALREADY_EXISTS {
                return Err(in_use());
            }
            let live_name = object_name("live", mp);
            // SAFETY: valid NUL-terminated name; this thread owns it.
            let live = unsafe { CreateMutexW(null(), TRUE, live_name.as_ptr()) };
            // SAFETY: no preconditions.
            let err = unsafe { GetLastError() };
            if live.is_null() || err == ERROR_ALREADY_EXISTS {
                if !live.is_null() {
                    // SAFETY: handle from CreateMutexW.
                    unsafe { CloseHandle(live) };
                }
                return Err(in_use());
            }
            let live = OwnedHandle(live);
            // SAFETY: unnamed manual-reset event.
            let done = unsafe { CreateEventW(null(), TRUE, FALSE, null()) };
            if done.is_null() {
                return Err(CliError::mount(
                    "Creating an event failed: try again.".to_string(),
                ));
            }
            Ok(Claim {
                mount_point: mount_point_arg(mp),
                stop,
                live,
                done: OwnedHandle(done),
            })
        }
    }

    impl Drop for Claim {
        fn drop(&mut self) {
            STOP.store(std::ptr::null_mut(), Ordering::SeqCst);
            // SAFETY: the live mutex was created owned by this thread (the
            // claim is created and dropped on the main thread); done is a
            // valid event.
            unsafe {
                ReleaseMutex(self.live.0);
                SetEvent(self.done.0);
            }
            // A Ctrl handler that loaded STOP or DONE just before may still
            // use them: leave both handles open until the process exits.
            for h in [&mut self.stop, &mut self.done] {
                h.0 = std::ptr::null_mut();
            }
        }
    }

    /// Console control handler. It runs on a thread the system creates, so
    /// it only touches the two event handles: SetEvent and
    /// WaitForSingleObject cannot unwind, and nothing here can panic.
    ///
    /// Ctrl-C/Ctrl-Break: signal stop and report the event as handled; the
    /// main thread unmounts and exits with 0. Close/logoff/shutdown: the
    /// process ends when this returns, so wait (bounded) for teardown.
    unsafe extern "system" fn on_ctrl(kind: u32) -> BOOL {
        let stop = STOP.load(Ordering::SeqCst);
        if stop.is_null() {
            return FALSE;
        }
        // SAFETY: STOP holds a live event handle while non-null.
        unsafe { SetEvent(stop) };
        if kind != CTRL_C_EVENT && kind != CTRL_BREAK_EVENT {
            let done = DONE.load(Ordering::SeqCst);
            if !done.is_null() {
                // SAFETY: DONE is never closed once published.
                unsafe { WaitForSingleObject(done, CLOSE_WAIT_MS) };
            }
        }
        TRUE
    }

    fn mount_failed(a: &MountArgs, backend: &Backend, e: winuxfs_core::Errno) -> CliError {
        CliError::mount(format!(
            "Backend '{}' could not mount {} ({}): check --type, --partition and --options.",
            backend.name(),
            a.device,
            errno_text(e)
        ))
    }

    /// Mounts, waits for stop, unmounts. Consumes the claim last.
    pub fn serve(
        a: &MountArgs,
        backend: &Backend,
        devs: OpenDevices,
        read_only: bool,
        claim: Claim,
    ) -> Result<(), CliError> {
        let fsp = WinFsp::load().map_err(|e| {
            CliError::mount(format!(
                "WinFsp could not be loaded ({e}): install WinFsp 2.x from https://winfsp.dev/rel/."
            ))
        })?;
        let mount = backend
            .mount(devs.devices(), &a.options)
            .map_err(|e| mount_failed(a, backend, e))?;
        let vfs = Arc::new(Vfs::new(
            Arc::new(mount),
            VfsOptions {
                case_insensitive: a.case_insensitive,
                read_only,
            },
        ));
        let opts = MountOptions {
            volume_label: backend.name().to_string(),
            read_only,
            case_sensitive: !a.case_insensitive,
        };
        // Publish the events before mounting so that a Ctrl-C arriving
        // right after the drive appears is not lost.
        STOP.store(claim.stop.0, Ordering::SeqCst);
        DONE.store(claim.done.0, Ordering::SeqCst);
        // SAFETY: on_ctrl is a valid handler for the life of the process.
        // The first call clears an inherited "ignore Ctrl-C" flag (set for
        // processes started in a new process group).
        unsafe {
            SetConsoleCtrlHandler(None, FALSE);
            SetConsoleCtrlHandler(Some(on_ctrl), TRUE);
        }
        let fs = mount_vfs(&fsp, vfs, &claim.mount_point, &opts).map_err(|e| {
            CliError::mount(format!(
                "Mounting on {} failed ({e}): make sure the mount point is free and WinFsp works.",
                claim.mount_point
            ))
        })?;
        println!(
            "Mounted {} ({}{}) on {}; press Ctrl-C or run 'winuxfs unmount {}' to unmount.",
            a.device,
            backend.name(),
            if read_only { ", read-only" } else { "" },
            claim.mount_point,
            claim.mount_point
        );
        let _ = std::io::stdout().flush();
        // SAFETY: the stop event lives as long as claim.
        unsafe { WaitForSingleObject(claim.stop.0, INFINITE) };
        // Ordered teardown: mount point and dispatcher (closing the Vfs
        // handles Windows still held), Vfs and backend Mount, devices.
        drop(fs);
        drop(devs);
        println!("Unmounted {}.", claim.mount_point);
        let _ = std::io::stdout().flush();
        drop(claim);
        Ok(())
    }

    pub fn unmount(mp: &str) -> Result<(), CliError> {
        let stop_name = object_name("stop", mp);
        // SAFETY: valid NUL-terminated name.
        let stop = unsafe { OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_name.as_ptr()) };
        if stop.is_null() {
            return Err(CliError::usage(format!(
                "Nothing is mounted by winuxfs on {mp} in this Windows session: check the \
                 mount point."
            )));
        }
        let stop = OwnedHandle(stop);
        let live_name = object_name("live", mp);
        // SAFETY: valid NUL-terminated name.
        let live = unsafe { OpenMutexW(SYNCHRONIZATION_SYNCHRONIZE, FALSE, live_name.as_ptr()) };
        // SAFETY: stop is a valid event handle with EVENT_MODIFY_STATE.
        unsafe { SetEvent(stop.0) };
        if live.is_null() {
            // The serving process is already gone.
            return Ok(());
        }
        let live = OwnedHandle(live);
        // SAFETY: valid mutex handle.
        match unsafe { WaitForSingleObject(live.0, UNMOUNT_TIMEOUT_MS) } {
            WAIT_OBJECT_0 | WAIT_ABANDONED => {
                // SAFETY: we own the mutex now.
                unsafe { ReleaseMutex(live.0) };
                println!("Unmounted {}.", mount_point_arg(mp));
                Ok(())
            }
            WAIT_TIMEOUT => Err(CliError::mount(format!(
                "{mp} did not unmount within {} s: close programs that use it, then try again.",
                UNMOUNT_TIMEOUT_MS / 1000
            ))),
            _ => Err(CliError::mount(format!(
                "Waiting for {mp} to unmount failed: try again."
            ))),
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn mount_point_forms() {
            assert_eq!(drive_letter("x:"), Some('X'));
            assert_eq!(drive_letter("X:\\"), Some('X'));
            assert_eq!(drive_letter("X:\\a"), None);
            assert_eq!(drive_letter("1:"), None);
            assert_eq!(key("x:"), "X:");
            assert_eq!(key("C:\\mnt\\a\\"), "C:/MNT/A");
        }
    }
}
