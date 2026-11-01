// SPDX-License-Identifier: GPL-2.0-only
//! `winuxfs service`: mounts from a config file, run by the Windows service
//! manager so that they appear at boot, in every session, and are unmounted
//! cleanly (log flushed) when the service stops or Windows shuts down.
//!
//! The config file (`winuxfs.conf` next to the executable by default) has
//! one mount per line, written exactly like the arguments of `winuxfs
//! mount`; empty lines and lines starting with `#` are ignored:
//!
//! ```text
//! Y: disk:1 --partition 1 --type xfs --case-insensitive
//! ```
//!
//! Without `--plugin-dir` the directory of the executable is searched.

use std::path::{Path, PathBuf};

use clap::{Parser, Subcommand};

use crate::cmd_mount::MountArgs;
use crate::common::CliError;

#[derive(Subcommand, Debug)]
pub enum ServiceCmd {
    /// Register the service (starts with Windows). Needs administrator rights.
    Install {
        /// Config file; default `winuxfs.conf` next to the executable (a
        /// commented template is written if it does not exist).
        #[arg(long, value_name = "FILE")]
        config: Option<PathBuf>,
    },
    /// Stop the service (unmounting everything) and remove it.
    Uninstall,
    /// Entry point for the service manager; not meant to be run by hand.
    #[command(hide = true)]
    Run {
        #[arg(long, value_name = "FILE")]
        config: PathBuf,
    },
}

pub fn run(cmd: ServiceCmd) -> Result<(), CliError> {
    #[cfg(windows)]
    {
        match cmd {
            ServiceCmd::Install { config } => win::install(config),
            ServiceCmd::Uninstall => win::uninstall(),
            ServiceCmd::Run { config } => win::run(&config),
        }
    }
    #[cfg(not(windows))]
    {
        let _ = cmd;
        Err(CliError::usage(
            "The service needs Windows: run winuxfs service on Windows.".to_string(),
        ))
    }
}

/// One config line parsed like the arguments of `winuxfs mount`.
#[derive(Parser, Debug)]
#[command(no_binary_name = true)]
struct Line {
    #[command(flatten)]
    mount: MountArgs,
}

/// Splits a line into arguments; `"double quotes"` group words with spaces.
fn split_args(line: &str) -> Result<Vec<String>, String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut in_word = false;
    let mut quoted = false;
    for c in line.chars() {
        match c {
            '"' => {
                quoted = !quoted;
                in_word = true;
            }
            c if c.is_whitespace() && !quoted => {
                if in_word {
                    out.push(std::mem::take(&mut cur));
                    in_word = false;
                }
            }
            c => {
                cur.push(c);
                in_word = true;
            }
        }
    }
    if quoted {
        return Err("unterminated quote".to_string());
    }
    if in_word {
        out.push(cur);
    }
    Ok(out)
}

/// Parses the config text. `default_plugin_dir` is used for mounts without
/// `--plugin-dir`. Errors name the line.
pub fn parse_config(
    text: &str,
    default_plugin_dir: Option<&Path>,
) -> Result<Vec<MountArgs>, String> {
    let mut mounts: Vec<MountArgs> = Vec::new();
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let args = split_args(line).map_err(|e| format!("line {}: {e}", n + 1))?;
        let mut m = Line::try_parse_from(args)
            .map_err(|e| format!("line {}: {}", n + 1, e.render()))?
            .mount;
        if m.plugin_dir.is_empty() {
            if let Some(d) = default_plugin_dir {
                m.plugin_dir.push(d.to_path_buf());
            }
        }
        if mounts
            .iter()
            .any(|o| o.mountpoint.eq_ignore_ascii_case(&m.mountpoint))
        {
            return Err(format!("line {}: {} is listed twice", n + 1, m.mountpoint));
        }
        mounts.push(m);
    }
    Ok(mounts)
}

const TEMPLATE: &str = "\
# winuxfs service: one mount per line, written like the arguments of
# `winuxfs mount`. Lines starting with # are ignored. Example:
#
#   Y: disk:1 --partition 1 --type xfs --case-insensitive
#
# Find the disk number with Get-Disk. After editing this file run
# `winuxfs service uninstall` and `winuxfs service install`, or restart
# the winuxfs service.
";

#[cfg(windows)]
mod win {
    use std::ffi::c_void;
    use std::io::Write;
    use std::path::{Path, PathBuf};
    use std::ptr::{null, null_mut};
    use std::sync::atomic::{AtomicPtr, Ordering};
    use std::sync::OnceLock;
    use std::time::Duration;

    use windows_sys::core::PWSTR;
    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, ERROR_ACCESS_DENIED, ERROR_SERVICE_DOES_NOT_EXIST,
        ERROR_SERVICE_EXISTS, FALSE, HANDLE, NO_ERROR, TRUE,
    };
    use windows_sys::Win32::Storage::FileSystem::DELETE;
    use windows_sys::Win32::System::Services::{
        ChangeServiceConfig2W, CloseServiceHandle, ControlService, CreateServiceW, DeleteService,
        OpenSCManagerW, OpenServiceW, QueryServiceStatus, RegisterServiceCtrlHandlerExW,
        SetServiceStatus, StartServiceCtrlDispatcherW, SC_MANAGER_CONNECT,
        SC_MANAGER_CREATE_SERVICE, SERVICE_ACCEPT_PRESHUTDOWN, SERVICE_ACCEPT_SHUTDOWN,
        SERVICE_ACCEPT_STOP, SERVICE_AUTO_START, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        SERVICE_CONFIG_DESCRIPTION, SERVICE_CONFIG_PRESHUTDOWN_INFO, SERVICE_CONTROL_INTERROGATE,
        SERVICE_CONTROL_PRESHUTDOWN, SERVICE_CONTROL_SHUTDOWN, SERVICE_CONTROL_STOP,
        SERVICE_DELAYED_AUTO_START_INFO, SERVICE_DESCRIPTIONW, SERVICE_ERROR_NORMAL,
        SERVICE_PRESHUTDOWN_INFO, SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START_PENDING,
        SERVICE_STATUS, SERVICE_STOP, SERVICE_STOPPED, SERVICE_STOP_PENDING, SERVICE_TABLE_ENTRYW,
        SERVICE_WIN32_OWN_PROCESS,
    };
    use windows_sys::Win32::System::Threading::{
        CreateEventW, SetEvent, WaitForSingleObject, INFINITE,
    };

    use super::{parse_config, TEMPLATE};
    use crate::cmd_mount::{self, MountArgs};
    use crate::common::CliError;

    const NAME: &str = "winuxfs";
    /// How long a stop may take (the unmounts run in parallel).
    const STOP_WAIT_MS: u32 = 60_000;
    /// A mount that fails is retried: the disk may not be ready at boot.
    const RETRIES: u32 = 24;
    const RETRY_DELAY_MS: u32 = 5_000;

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    fn fail(what: &str) -> CliError {
        // SAFETY: no preconditions.
        let e = unsafe { GetLastError() };
        if e == ERROR_ACCESS_DENIED {
            return CliError {
                code: crate::common::EXIT_PRIVILEGE,
                msg: format!("{what} needs administrator rights: run from an elevated PowerShell."),
            };
        }
        CliError::mount(format!("{what} failed (Windows error {e})."))
    }

    fn exe_dir() -> Result<(PathBuf, PathBuf), CliError> {
        let exe = std::env::current_exe()
            .map_err(|e| CliError::usage(format!("Cannot find the executable: {e}.")))?;
        let dir = exe.parent().map(Path::to_path_buf).unwrap_or_default();
        Ok((exe, dir))
    }

    struct Sc(*mut c_void);
    impl Drop for Sc {
        fn drop(&mut self) {
            if !self.0.is_null() {
                // SAFETY: a handle from OpenSCManager/CreateService/OpenService.
                unsafe { CloseServiceHandle(self.0) };
            }
        }
    }

    pub fn install(config: Option<PathBuf>) -> Result<(), CliError> {
        let (exe, dir) = exe_dir()?;
        let config = config.unwrap_or_else(|| dir.join("winuxfs.conf"));
        let config = if config.is_absolute() {
            config
        } else {
            std::env::current_dir().unwrap_or_default().join(config)
        };
        if !config.exists() {
            std::fs::write(&config, TEMPLATE)
                .map_err(|e| CliError::usage(format!("Cannot write {}: {e}.", config.display())))?;
            println!(
                "Wrote a template {}: add your mounts there.",
                config.display()
            );
        }
        // Check the file now so mistakes show up here, not in the log.
        let text = std::fs::read_to_string(&config)
            .map_err(|e| CliError::usage(format!("Cannot read {}: {e}.", config.display())))?;
        let n = parse_config(&text, Some(&dir))
            .map_err(|e| CliError::usage(format!("{}: {e}", config.display())))?
            .len();
        let bin = format!(
            "\"{}\" service run --config \"{}\"",
            exe.display(),
            config.display()
        );
        // SAFETY: valid arguments; handles are closed by Sc.
        unsafe {
            let scm = Sc(OpenSCManagerW(null(), null(), SC_MANAGER_CREATE_SERVICE));
            if scm.0.is_null() {
                return Err(fail("Opening the service manager"));
            }
            // WinFsp.Launcher gives session-0 file systems their global drive
            // letters, so the service starts after it.
            let deps = wide("WinFsp.Launcher\0");
            let svc = Sc(CreateServiceW(
                scm.0,
                wide(NAME).as_ptr(),
                wide("winuxfs file system mounts").as_ptr(),
                SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE | 0x0002, /* SERVICE_CHANGE_CONFIG */
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,
                wide(&bin).as_ptr(),
                null(),
                null_mut(),
                deps.as_ptr(),
                null(),
                null(),
            ));
            if svc.0.is_null() {
                if GetLastError() == ERROR_SERVICE_EXISTS {
                    return Err(CliError::usage(
                        "The winuxfs service is already installed: run 'winuxfs service \
                         uninstall' first."
                            .to_string(),
                    ));
                }
                return Err(fail("Creating the service"));
            }
            let mut desc = wide("Mounts Linux file systems from winuxfs.conf at boot (winuxfs).");
            let d = SERVICE_DESCRIPTIONW {
                lpDescription: desc.as_mut_ptr(),
            };
            ChangeServiceConfig2W(
                svc.0,
                SERVICE_CONFIG_DESCRIPTION,
                &d as *const _ as *const c_void,
            );
            let delayed = SERVICE_DELAYED_AUTO_START_INFO {
                fDelayedAutostart: TRUE,
            };
            ChangeServiceConfig2W(
                svc.0,
                SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                &delayed as *const _ as *const c_void,
            );
            // Time Windows waits for the unmounts at shutdown.
            let pre = SERVICE_PRESHUTDOWN_INFO {
                dwPreshutdownTimeout: STOP_WAIT_MS,
            };
            ChangeServiceConfig2W(
                svc.0,
                SERVICE_CONFIG_PRESHUTDOWN_INFO,
                &pre as *const _ as *const c_void,
            );
        }
        println!(
            "Installed the winuxfs service ({n} mount{} in {}). It starts with Windows; \
             start it now with: sc.exe start winuxfs",
            if n == 1 { "" } else { "s" },
            config.display()
        );
        Ok(())
    }

    pub fn uninstall() -> Result<(), CliError> {
        // SAFETY: valid arguments; handles are closed by Sc.
        unsafe {
            let scm = Sc(OpenSCManagerW(null(), null(), SC_MANAGER_CONNECT));
            if scm.0.is_null() {
                return Err(fail("Opening the service manager"));
            }
            let svc = Sc(OpenServiceW(
                scm.0,
                wide(NAME).as_ptr(),
                SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE,
            ));
            if svc.0.is_null() {
                if GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST {
                    return Err(CliError::usage(
                        "The winuxfs service is not installed.".to_string(),
                    ));
                }
                return Err(fail("Opening the service"));
            }
            let mut st: SERVICE_STATUS = std::mem::zeroed();
            if ControlService(svc.0, SERVICE_CONTROL_STOP, &mut st) != 0 {
                // Wait for the stop (the mounts are unmounted first).
                for _ in 0..(STOP_WAIT_MS / 500 + 10) {
                    if QueryServiceStatus(svc.0, &mut st) == 0
                        || st.dwCurrentState == SERVICE_STOPPED
                    {
                        break;
                    }
                    std::thread::sleep(Duration::from_millis(500));
                }
            }
            if DeleteService(svc.0) == 0 {
                return Err(fail("Removing the service"));
            }
        }
        println!("Removed the winuxfs service.");
        Ok(())
    }

    // ---- service side -------------------------------------------------

    static CONFIG: OnceLock<PathBuf> = OnceLock::new();
    static STATUS: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
    static STOP_EVENT: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

    pub fn run(config: &Path) -> Result<(), CliError> {
        let _ = CONFIG.set(config.to_path_buf());
        let name = wide(NAME);
        let table = [
            SERVICE_TABLE_ENTRYW {
                lpServiceName: name.as_ptr() as PWSTR,
                lpServiceProc: Some(service_main),
            },
            SERVICE_TABLE_ENTRYW {
                lpServiceName: null_mut(),
                lpServiceProc: None,
            },
        ];
        // SAFETY: the table is valid and NUL-terminated; this blocks until
        // the service has stopped.
        if unsafe { StartServiceCtrlDispatcherW(table.as_ptr()) } == 0 {
            return Err(CliError::usage(
                "winuxfs service run is started by Windows: use 'winuxfs service install' \
                 and 'sc.exe start winuxfs'."
                    .to_string(),
            ));
        }
        Ok(())
    }

    fn report(state: u32, accepted: u32, checkpoint: u32, wait_hint: u32) {
        let h = STATUS.load(Ordering::SeqCst);
        if h.is_null() {
            return;
        }
        let st = SERVICE_STATUS {
            dwServiceType: SERVICE_WIN32_OWN_PROCESS,
            dwCurrentState: state,
            dwControlsAccepted: accepted,
            dwWin32ExitCode: NO_ERROR,
            dwServiceSpecificExitCode: 0,
            dwCheckPoint: checkpoint,
            dwWaitHint: wait_hint,
        };
        // SAFETY: a valid status handle and structure.
        unsafe { SetServiceStatus(h, &st) };
    }

    fn log(msg: &str) {
        let Some(cfg) = CONFIG.get() else { return };
        let path = cfg.with_file_name("winuxfs-service.log");
        if let Ok(mut f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
        {
            let secs = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0);
            let _ = writeln!(f, "[{secs}] {msg}");
        }
    }

    unsafe extern "system" fn handler(
        control: u32,
        _event: u32,
        _data: *mut c_void,
        _ctx: *mut c_void,
    ) -> u32 {
        match control {
            SERVICE_CONTROL_STOP | SERVICE_CONTROL_SHUTDOWN | SERVICE_CONTROL_PRESHUTDOWN => {
                report(SERVICE_STOP_PENDING, 0, 1, STOP_WAIT_MS);
                let e = STOP_EVENT.load(Ordering::SeqCst);
                if !e.is_null() {
                    // SAFETY: the event lives until the service has stopped.
                    unsafe { SetEvent(e) };
                }
                NO_ERROR
            }
            SERVICE_CONTROL_INTERROGATE => NO_ERROR,
            _ => 1, // ERROR_INVALID_FUNCTION
        }
    }

    fn stopping() -> bool {
        let e = STOP_EVENT.load(Ordering::SeqCst);
        // SAFETY: a valid event handle.
        !e.is_null() && unsafe { WaitForSingleObject(e, 0) } == 0
    }

    /// Waits up to `ms`; true if the service was asked to stop meanwhile.
    fn wait_stop(ms: u32) -> bool {
        let e = STOP_EVENT.load(Ordering::SeqCst);
        // SAFETY: a valid event handle.
        unsafe { WaitForSingleObject(e, ms) == 0 }
    }

    unsafe extern "system" fn service_main(_argc: u32, _argv: *mut PWSTR) {
        let name = wide(NAME);
        // SAFETY: valid name and handler.
        let h = unsafe { RegisterServiceCtrlHandlerExW(name.as_ptr(), Some(handler), null()) };
        if h.is_null() {
            return;
        }
        STATUS.store(h, Ordering::SeqCst);
        report(SERVICE_START_PENDING, 0, 1, 10_000);
        // SAFETY: plain event creation.
        let ev: HANDLE = unsafe { CreateEventW(null(), TRUE, FALSE, null()) };
        STOP_EVENT.store(ev, Ordering::SeqCst);

        let mounts = load_mounts();
        report(
            SERVICE_RUNNING,
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN,
            0,
            0,
        );
        let points: Vec<String> = mounts.iter().map(|m| m.mountpoint.clone()).collect();
        let threads: Vec<_> = mounts
            .into_iter()
            .map(|m| std::thread::spawn(move || mount_loop(m)))
            .collect();

        // SAFETY: a valid event handle.
        unsafe { WaitForSingleObject(ev, INFINITE) };
        log("stopping: unmounting");
        // Unmount every mount point at once; each waits until its serving
        // thread has flushed and released the devices.
        std::thread::scope(|s| {
            for mp in &points {
                s.spawn(move || {
                    if let Err(e) = cmd_mount::unmount(mp) {
                        log(&format!("unmount {mp}: {}", e.msg));
                    }
                });
            }
        });
        for t in threads {
            let _ = t.join();
        }
        log("stopped");
        report(SERVICE_STOPPED, 0, 0, 0);
        // SAFETY: closing our own event after the last use.
        unsafe { CloseHandle(ev) };
    }

    fn load_mounts() -> Vec<MountArgs> {
        let Some(cfg) = CONFIG.get() else {
            return Vec::new();
        };
        let dir = cfg.parent().map(Path::to_path_buf);
        let text = match std::fs::read_to_string(cfg) {
            Ok(t) => t,
            Err(e) => {
                log(&format!("cannot read {}: {e}", cfg.display()));
                return Vec::new();
            }
        };
        match parse_config(
            &text,
            std::env::current_exe()
                .ok()
                .as_deref()
                .and_then(Path::parent)
                .or(dir.as_deref()),
        ) {
            Ok(m) => {
                log(&format!("{} mount(s) configured", m.len()));
                m
            }
            Err(e) => {
                log(&format!("config error: {e}"));
                Vec::new()
            }
        }
    }

    /// Mounts (retrying while the disk is not ready) and serves until the
    /// service is stopped.
    fn mount_loop(m: MountArgs) {
        for attempt in 1..=RETRIES {
            if stopping() {
                return;
            }
            match cmd_mount::mount(&m) {
                Ok(()) => {
                    log(&format!("{} unmounted", m.mountpoint));
                    return;
                }
                Err(e) => {
                    log(&format!(
                        "mount {} attempt {attempt}/{RETRIES}: {}",
                        m.mountpoint, e.msg
                    ));
                    if wait_stop(RETRY_DELAY_MS) {
                        return;
                    }
                }
            }
        }
        log(&format!("giving up on {}", m.mountpoint));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_mount_lines_and_skips_comments() {
        let text =
            "# c\n\nY: disk:1 --partition 1 --type xfs --case-insensitive\n  Z: disk:2 --ro  \n";
        let m = parse_config(text, Some(Path::new("C:\\w"))).unwrap();
        assert_eq!(m.len(), 2);
        assert_eq!(m[0].mountpoint, "Y:");
        assert_eq!(m[0].device, "disk:1");
        assert_eq!(m[0].partition, Some(1));
        assert!(m[0].case_insensitive && !m[0].ro);
        assert!(m[1].ro);
        assert_eq!(m[1].plugin_dir, vec![PathBuf::from("C:\\w")]);
    }

    #[test]
    fn explicit_plugin_dir_wins() {
        let m = parse_config("Y: disk:1 --plugin-dir D:\\p", Some(Path::new("C:\\w"))).unwrap();
        assert_eq!(m[0].plugin_dir, vec![PathBuf::from("D:\\p")]);
    }

    #[test]
    fn quotes_group_words() {
        let m = parse_config("Y: \"D:\\my images\\a.img\" --type xfs", None).unwrap();
        assert_eq!(m[0].device, "D:\\my images\\a.img");
        assert!(split_args("a \"b").is_err());
    }

    #[test]
    fn errors_name_the_line() {
        let e = parse_config("Y: disk:1\nQ:\n", None).unwrap_err();
        assert!(e.starts_with("line 2:"), "{e}");
        let e = parse_config("Y: disk:1 --nonsense", None).unwrap_err();
        assert!(e.starts_with("line 1:"), "{e}");
        let e = parse_config("Y: disk:1\ny: disk:2\n", None).unwrap_err();
        assert!(e.contains("listed twice"), "{e}");
    }

    #[test]
    fn template_is_all_comments() {
        assert!(parse_config(TEMPLATE, None).unwrap().is_empty());
    }
}
