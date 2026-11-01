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
use winuxfs_core::vfs::VfsOptions;
use winuxfs_core::BlockDevice;
use winuxfs_win::MountOptions;

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

/// The Vfs and WinFsp settings of a mount; `--ro` (or a read-only
/// backend) turns on both read-only flags, and the case mode must agree
/// between the two.
pub fn mount_settings(
    label: &str,
    read_only: bool,
    case_insensitive: bool,
) -> (VfsOptions, MountOptions) {
    (
        VfsOptions {
            case_insensitive,
            read_only,
        },
        MountOptions {
            volume_label: label.to_string(),
            read_only,
            case_sensitive: !case_insensitive,
        },
    )
}

/// Refuses `--log`/`--rt` naming the same device as the data device or
/// each other: the second open would fail as "busy", which misleads.
fn check_device_roles(a: &MountArgs) -> Result<(), CliError> {
    let same = |x: &str, y: &str| x.eq_ignore_ascii_case(y) && !x.eq_ignore_ascii_case("none");
    let roles = [
        ("the data device", Some(&a.device)),
        ("--log", a.log.as_ref()),
        ("--rt", a.rt.as_ref()),
    ];
    for (i, (n1, d1)) in roles.iter().enumerate() {
        for (n2, d2) in &roles[i + 1..] {
            if let (Some(d1), Some(d2)) = (d1, d2) {
                if same(d1, d2) {
                    return Err(CliError::usage(format!(
                        "{n2} names the same device as {n1} ({d1}): give each role its own \
                         device (partitions cannot be chosen for --log/--rt yet)."
                    )));
                }
            }
        }
    }
    Ok(())
}

/// Runs `winuxfs mount`. On Windows it returns after the file system was
/// unmounted again.
pub fn mount(a: &MountArgs) -> Result<(), CliError> {
    check_device_roles(a)?;
    // Claim the mount point first: if it is taken, nothing is opened. The
    // claim also installs the Ctrl-C handler, so an interrupt during the
    // device opens or the backend mount still tears down in order.
    #[cfg(windows)]
    let claim = win::Claim::new(&a.mountpoint)?;
    // Everything `run` opens is dropped when it returns, before the claim
    // is released: a waiting `unmount` or a new `mount` of the same mount
    // point never sees "finished" while a disk is still offline.
    let r = run(
        a,
        #[cfg(windows)]
        &claim,
    );
    #[cfg(windows)]
    drop(claim);
    r
}

fn run(a: &MountArgs, #[cfg(windows)] claim: &win::Claim) -> Result<(), CliError> {
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
    // Probing (`--type auto`) only reads, so the devices are opened
    // read-only for it and reopened read-write afterwards if needed. Off
    // Windows nothing is mounted, so nothing may be written either.
    // `--options ro` (the backend's own flag) must make the volume read-only for
    // Windows too, or the filesystem looks writable and every write fails late.
    let opt_ro = a.options.split(',').rev().find_map(|o| match o.trim() {
        "ro" => Some(true),
        "rw" => Some(false),
        _ => None,
    }) == Some(true);
    let first_ro =
        a.ro || opt_ro || auto || named.is_some_and(is_readonly_backend) || cfg!(not(windows));
    let devs = open_all(a, first_ro)?;
    let backend = match named {
        Some(b) => b,
        None => probe_one(&backends, &devs, &a.device)?,
    };
    let read_only = a.ro || opt_ro || is_readonly_backend(backend);
    let (vfs_opts, mount_opts) = mount_settings(backend.name(), read_only, a.case_insensitive);

    #[cfg(not(windows))]
    {
        let _ = (devs, vfs_opts, mount_opts);
        Err(CliError::usage(format!(
            "Mounting needs Windows with WinFsp 2.x (the arguments are valid and \
             '{}' would serve {}{}): run winuxfs mount on Windows.",
            backend.name(),
            a.device,
            if read_only { " read-only" } else { "" }
        )))
    }
    #[cfg(windows)]
    {
        let devs = if first_ro && !read_only {
            drop(devs);
            open_all(a, false)?
        } else {
            devs
        };
        win::serve(a, backend, devs, vfs_opts, mount_opts, claim)
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
    //! keyed by the normalised mount point: a "live" mutex the mounting
    //! thread owns from the claim until teardown has finished (an exiting
    //! or killed process abandons it), and a manual-reset "stop" event that
    //! Ctrl-C and `winuxfs unmount` signal.
    //!
    //! Ownership of the live mutex is what claims the mount point. A stop
    //! event that already exists while we own the mutex is stale (left by
    //! a process that finished its teardown and has not exited yet): it is
    //! reset and reused, so `unmount X: && mount X: ...` never races.
    //!
    //! Names are `Global\winuxfs-{live,stop}-<KEY>` (the global namespace, so a
    //! mount served by the Windows service in session 0 can be unmounted from
    //! an administrator shell in any session), KEY being the mount
    //! point upper-cased with `\` replaced by `/`. Kernel object names are
    //! limited to MAX_PATH (260) characters, so mount points whose name
    //! would be longer are refused.

    use std::ffi::c_void;
    use std::io::Write;
    use std::path::Path;
    use std::ptr::null;
    use std::sync::atomic::{AtomicPtr, Ordering};
    use std::sync::Arc;

    use windows_sys::core::BOOL;
    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, ERROR_ACCESS_DENIED, ERROR_ALREADY_EXISTS, FALSE, HANDLE, TRUE,
        WAIT_ABANDONED, WAIT_OBJECT_0, WAIT_TIMEOUT,
    };
    use windows_sys::Win32::Storage::FileSystem::GetLogicalDrives;
    use windows_sys::Win32::System::Console::{
        SetConsoleCtrlHandler, CTRL_BREAK_EVENT, CTRL_C_EVENT,
    };
    use windows_sys::Win32::System::Threading::{
        CreateEventW, CreateMutexW, OpenEventW, OpenMutexW, ReleaseMutex, ResetEvent, SetEvent,
        WaitForSingleObject, EVENT_MODIFY_STATE, INFINITE, SYNCHRONIZATION_SYNCHRONIZE,
    };
    use winuxfs_core::plugin::Backend;
    use winuxfs_core::vfs::{Vfs, VfsOptions};
    use winuxfs_win::{mount_vfs, MountOptions, WinFsp};

    use super::{MountArgs, OpenDevices};
    use crate::common::{errno_text, finish_vfs, CliError, EXIT_PRIVILEGE};

    /// How long `unmount` waits for the serving process to finish.
    const UNMOUNT_TIMEOUT_MS: u32 = 30_000;
    /// How long the console-close handler waits for teardown (Windows ends
    /// the process a few seconds after such an event anyway).
    const CLOSE_WAIT_MS: u32 = 10_000;
    /// Longest object name accepted (MAX_PATH including the terminator).
    const MAX_OBJECT_NAME: usize = 259;

    /// The stop event of the mount in this process, for the Ctrl handler.
    static STOP: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
    /// Signalled when teardown has finished, for the Ctrl handler.
    static DONE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    /// Prints a line; a closed console (after CTRL_CLOSE) is ignored.
    fn say(msg: std::fmt::Arguments<'_>) {
        let mut out = std::io::stdout();
        let _ = writeln!(out, "{msg}");
        let _ = out.flush();
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

    /// A checked mount point: the form handed to WinFsp (`X:` or an
    /// absolute directory path) and its object-name key.
    #[derive(Debug, PartialEq, Eq)]
    struct MountPoint {
        arg: String,
        key: String,
    }

    fn mount_point(mp: &str) -> Result<MountPoint, CliError> {
        if mp.is_empty() || mp.contains('\0') {
            return Err(CliError::usage(
                "The mount point is empty or invalid: give a drive letter such as X:.",
            ));
        }
        let arg = match drive_letter(mp) {
            Some(l) => format!("{l}:"),
            None if mp.len() == 2 && mp.ends_with(':') => {
                return Err(CliError::usage(format!(
                    "{mp} is not a drive letter: use a letter from A: to Z:."
                )))
            }
            None if !Path::new(mp).is_absolute() => {
                return Err(CliError::usage(format!(
                    "Directory mount point {mp} is relative: give an absolute path such \
                     as C:\\mnt\\linux."
                )))
            }
            None => mp.trim_end_matches(['\\', '/']).to_string(),
        };
        let key = arg.to_uppercase().replace('\\', "/");
        if "Global\\winuxfs-stop-".len() + key.encode_utf16().count() > MAX_OBJECT_NAME {
            return Err(CliError::usage(format!(
                "Mount point {mp} is too long: use a drive letter or a path of at most \
                 about 230 characters."
            )));
        }
        Ok(MountPoint { arg, key })
    }

    fn object_name(kind: &str, mp: &MountPoint) -> Vec<u16> {
        wide(&format!("Global\\winuxfs-{kind}-{}", mp.key))
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

    /// The claimed mount point: the owned live mutex and the stop event.
    /// Created and dropped on the main thread (mutex ownership is per
    /// thread).
    pub struct Claim {
        mount_point: String,
        live: OwnedHandle,
        stop: OwnedHandle,
        done: OwnedHandle,
    }

    impl Claim {
        /// Fails with exit code 2, before anything else is touched, if the
        /// mount point is in use. On success the Ctrl handler is installed.
        pub fn new(mp_text: &str) -> Result<Claim, CliError> {
            let mp = mount_point(mp_text)?;
            let in_use = || {
                CliError::mount(format!(
                    "{mp_text} is already in use: choose a free drive letter or unmount it first."
                ))
            };
            match drive_letter(&mp.arg) {
                Some(l) => {
                    // SAFETY: no preconditions.
                    let mask = unsafe { GetLogicalDrives() };
                    if mask & (1 << (l as u8 - b'A')) != 0 {
                        return Err(in_use());
                    }
                }
                None => {
                    if Path::new(&mp.arg).exists() {
                        return Err(CliError::mount(format!(
                            "{mp_text} already exists: give a directory path that does not \
                             exist yet (WinFsp creates it)."
                        )));
                    }
                }
            }
            let live_name = object_name("live", &mp);
            // SAFETY: valid NUL-terminated name; owned by this thread when
            // newly created.
            let live = unsafe { CreateMutexW(null(), TRUE, live_name.as_ptr()) };
            // SAFETY: no preconditions.
            let err = unsafe { GetLastError() };
            if live.is_null() {
                return Err(if err == ERROR_ACCESS_DENIED {
                    elevated(mp_text)
                } else {
                    in_use()
                });
            }
            let live = OwnedHandle(live);
            if err == ERROR_ALREADY_EXISTS {
                // Someone else created it: ours only if it is free
                // (released or abandoned after a finished mount).
                // SAFETY: valid mutex handle.
                match unsafe { WaitForSingleObject(live.0, 0) } {
                    WAIT_OBJECT_0 | WAIT_ABANDONED => {}
                    _ => return Err(in_use()),
                }
            }
            // From here on this thread owns the live mutex; dropping the
            // partial claim must release it.
            let release = |e: CliError| {
                // SAFETY: owned by this thread (see above).
                unsafe { ReleaseMutex(live.0) };
                e
            };
            let stop_name = object_name("stop", &mp);
            // SAFETY: valid NUL-terminated name; manual reset, not signalled.
            let stop = unsafe { CreateEventW(null(), TRUE, FALSE, stop_name.as_ptr()) };
            if stop.is_null() {
                return Err(release(in_use()));
            }
            let stop = OwnedHandle(stop);
            // An existing event is stale (we own the mutex): clear it.
            // SAFETY: valid event handle.
            unsafe { ResetEvent(stop.0) };
            // SAFETY: unnamed manual-reset event.
            let done = unsafe { CreateEventW(null(), TRUE, FALSE, null()) };
            if done.is_null() {
                return Err(release(CliError::mount(
                    "Creating an event failed: try again.".to_string(),
                )));
            }
            let claim = Claim {
                mount_point: mp.arg,
                live,
                stop,
                done: OwnedHandle(done),
            };
            STOP.store(claim.stop.0, Ordering::SeqCst);
            DONE.store(claim.done.0, Ordering::SeqCst);
            // SAFETY: on_ctrl is valid for the life of the process. The
            // first call clears an inherited "ignore Ctrl-C" flag (set for
            // processes started in a new process group).
            unsafe {
                SetConsoleCtrlHandler(None, FALSE);
                SetConsoleCtrlHandler(Some(on_ctrl), TRUE);
            }
            Ok(claim)
        }

        /// Whether Ctrl-C or `unmount` asked to stop.
        fn stop_requested(&self) -> bool {
            // SAFETY: valid event handle.
            unsafe { WaitForSingleObject(self.stop.0, 0) == WAIT_OBJECT_0 }
        }
    }

    impl Drop for Claim {
        fn drop(&mut self) {
            STOP.store(std::ptr::null_mut(), Ordering::SeqCst);
            // SAFETY: the live mutex is owned by this thread (the claim is
            // created and dropped on the main thread); done is valid.
            unsafe {
                ReleaseMutex(self.live.0);
                SetEvent(self.done.0);
            }
            // A Ctrl handler that loaded STOP or DONE just before may still
            // use them: leave both handles open until the process exits (a
            // later mount treats the left-over stop event as stale).
            for h in [&mut self.stop, &mut self.done] {
                h.0 = std::ptr::null_mut();
            }
        }
    }

    fn elevated(mp: &str) -> CliError {
        CliError {
            code: EXIT_PRIVILEGE,
            msg: format!(
                "Access to the winuxfs mount on {mp} was denied: it was started from an \
                 elevated prompt, so run this command from an elevated (Run as \
                 administrator) prompt too."
            ),
        }
    }

    /// Console control handler. It runs on a thread the system creates, so
    /// it only touches the two event handles: SetEvent and
    /// WaitForSingleObject cannot unwind, and nothing here can panic.
    ///
    /// Ctrl-C/Ctrl-Break: signal stop and report the event as handled; the
    /// main thread unmounts and exits with 0 (or EXIT_UNMOUNT if the final
    /// write-back failed). Close/logoff/shutdown: the
    /// process ends when this returns, so wait (bounded) for teardown.
    unsafe extern "system" fn on_ctrl(kind: u32) -> BOOL {
        let stop = STOP.load(Ordering::SeqCst);
        if stop.is_null() {
            return FALSE;
        }
        // SAFETY: STOP holds an event handle that is never closed while the
        // process runs once published.
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

    fn cancelled(claim: &Claim) -> Result<(), CliError> {
        say(format_args!(
            "Stopped before {} was mounted.",
            claim.mount_point
        ));
        Ok(())
    }

    /// Mounts, waits for stop, unmounts. A stop request before the mount
    /// point exists ends with exit 0 as well. `devs` (a parameter, so
    /// dropped after every local) goes last; the caller releases the claim.
    pub fn serve(
        a: &MountArgs,
        backend: &Backend,
        devs: OpenDevices,
        vfs_opts: VfsOptions,
        mount_opts: MountOptions,
        claim: &Claim,
    ) -> Result<(), CliError> {
        let fsp = WinFsp::load().map_err(|e| {
            CliError::mount(format!(
                "WinFsp could not be loaded ({e}): install WinFsp 2.x from https://winfsp.dev/rel/."
            ))
        })?;
        if claim.stop_requested() {
            return cancelled(claim);
        }
        let mount = backend
            .mount(devs.devices(), &a.options)
            .map_err(|e| mount_failed(a, backend, e))?;
        let vfs = Arc::new(Vfs::new(Arc::new(mount), vfs_opts));
        // The adapter holds one reference; ours ends the session below.
        let session = vfs.clone();
        if claim.stop_requested() {
            return cancelled(claim);
        }
        let fs = mount_vfs(&fsp, vfs, &claim.mount_point, &mount_opts).map_err(|e| {
            CliError::mount(format!(
                "Mounting on {} failed ({e}): make sure the mount point is free and WinFsp works.",
                claim.mount_point
            ))
        })?;
        say(format_args!(
            "Mounted {} ({}{}) on {}; press Ctrl-C or run 'winuxfs unmount {}' to unmount.",
            a.device,
            backend.name(),
            if mount_opts.read_only {
                ", read-only"
            } else {
                ""
            },
            claim.mount_point,
            claim.mount_point
        ));
        // SAFETY: the stop event lives as long as claim.
        unsafe { WaitForSingleObject(claim.stop.0, INFINITE) };
        // Ordered teardown: mount point and dispatcher (closing the Vfs
        // handles Windows still held), Vfs and backend Mount, devices.
        drop(fs);
        // The dispatcher is stopped and the handles are closed: now write
        // everything back. A failure is the exit code, but the devices are
        // still released.
        let r = finish_vfs(&session, &claim.mount_point);
        drop(session);
        drop(devs);
        r?;
        say(format_args!("Unmounted {}.", claim.mount_point));
        Ok(())
    }

    /// The error for a mount point that no winuxfs process serves.
    pub(super) fn nothing_mounted(mp_text: &str) -> CliError {
        CliError::usage(format!(
            "Nothing is mounted by winuxfs on {mp_text}: check the mount point."
        ))
    }

    pub fn unmount(mp_text: &str) -> Result<(), CliError> {
        let mp = mount_point(mp_text)?;
        let stop_name = object_name("stop", &mp);
        // SAFETY: valid NUL-terminated name.
        let stop = unsafe { OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_name.as_ptr()) };
        if stop.is_null() {
            // SAFETY: no preconditions.
            if unsafe { GetLastError() } == ERROR_ACCESS_DENIED {
                return Err(elevated(mp_text));
            }
            return Err(nothing_mounted(mp_text));
        }
        let stop = OwnedHandle(stop);
        let live_name = object_name("live", &mp);
        // SAFETY: valid NUL-terminated name.
        let live = unsafe { OpenMutexW(SYNCHRONIZATION_SYNCHRONIZE, FALSE, live_name.as_ptr()) };
        // SAFETY: stop is a valid event handle with EVENT_MODIFY_STATE.
        unsafe { SetEvent(stop.0) };
        if live.is_null() {
            // The serving process is gone and only a stale (leaked) stop
            // event is left: nothing to unmount.
            return Err(nothing_mounted(&mp.arg));
        }
        let live = OwnedHandle(live);
        // SAFETY: valid mutex handle.
        match unsafe { WaitForSingleObject(live.0, UNMOUNT_TIMEOUT_MS) } {
            WAIT_OBJECT_0 | WAIT_ABANDONED => {
                // SAFETY: we own the mutex now.
                unsafe { ReleaseMutex(live.0) };
                say(format_args!("Unmounted {}.", mp.arg));
                Ok(())
            }
            WAIT_TIMEOUT => Err(CliError::mount(format!(
                "{mp_text} did not unmount within {} s: close programs that use it, then try again.",
                UNMOUNT_TIMEOUT_MS / 1000
            ))),
            _ => Err(CliError::mount(format!(
                "Waiting for {mp_text} to unmount failed: try again."
            ))),
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn nothing_mounted_message_and_code() {
            let e = nothing_mounted("X:");
            assert!(e.msg.starts_with("Nothing is mounted by winuxfs on X:"));
            assert_eq!(e.code, 1);
        }

        #[test]
        fn mount_point_forms() {
            assert_eq!(drive_letter("x:"), Some('X'));
            assert_eq!(drive_letter("X:\\"), Some('X'));
            assert_eq!(drive_letter("X:\\a"), None);
            assert_eq!(drive_letter("1:"), None);
            assert_eq!(mount_point("x:").unwrap().key, "X:");
            let d = mount_point("C:\\mnt\\a\\").unwrap();
            assert_eq!((d.arg.as_str(), d.key.as_str()), ("C:\\mnt\\a", "C:/MNT/A"));
            assert_eq!(mount_point("mnt\\a").unwrap_err().code, 1);
            assert_eq!(mount_point("1:").unwrap_err().code, 1);
            let long = format!("C:\\{}", "a".repeat(300));
            assert_eq!(mount_point(&long).unwrap_err().code, 1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ro_turns_on_both_read_only_flags() {
        let (v, m) = mount_settings("memfs", true, false);
        assert!(v.read_only && m.read_only);
        assert!(!v.case_insensitive && m.case_sensitive);
        assert_eq!(m.volume_label, "memfs");
        let (v, m) = mount_settings("memfs", false, true);
        assert!(!v.read_only && !m.read_only);
        assert!(v.case_insensitive && !m.case_sensitive);
    }
}
