// SPDX-License-Identifier: GPL-2.0-only
//! Pieces shared by the subcommands: errors and exit codes, plugin
//! directories, device opening and partition selection.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use winuxfs_core::part::{scan, PartitionInfo, PartitionView};
use winuxfs_core::{BlockDevice, Errno};

/// Exit code: wrong arguments, unknown names, unusable devices.
pub const EXIT_USAGE: i32 = 1;
/// Exit code: the mount itself failed.
pub const EXIT_MOUNT: i32 = 2;
/// Exit code: the user lacks the rights to open a device.
pub const EXIT_PRIVILEGE: i32 = 3;

/// Environment variable with extra plugin directories (a `PATH`-style list).
pub const PLUGIN_PATH_ENV: &str = "WINUXFS_PLUGIN_PATH";

/// A failure to report: one sentence for stderr and the exit code.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CliError {
    pub code: i32,
    pub msg: String,
}

impl CliError {
    pub fn usage(msg: impl Into<String>) -> CliError {
        CliError {
            code: EXIT_USAGE,
            msg: msg.into(),
        }
    }

    pub fn mount(msg: impl Into<String>) -> CliError {
        CliError {
            code: EXIT_MOUNT,
            msg: msg.into(),
        }
    }
}

/// A few words for an errno, for messages.
pub fn errno_text(e: Errno) -> String {
    let s = match e {
        Errno::ENOENT => "not found",
        Errno::EIO => "I/O error",
        Errno::EACCES => "access denied",
        Errno::EBUSY => "device or resource busy",
        Errno::ENODEV => "no such device",
        Errno::EINVAL => "invalid argument",
        Errno::ENOSPC => "no space left",
        Errno::EROFS => "read-only device",
        Errno::ENOSYS => "not supported",
        _ => return format!("error {}", e.0),
    };
    s.to_string()
}

/// Plugin directories: the `--plugin-dir` values when any are given,
/// otherwise `WINUXFS_PLUGIN_PATH` followed by `plugins/` next to the
/// executable.
pub fn plugin_dirs(explicit: &[PathBuf]) -> Vec<PathBuf> {
    if !explicit.is_empty() {
        return explicit.to_vec();
    }
    let mut dirs: Vec<PathBuf> = std::env::var_os(PLUGIN_PATH_ENV)
        .map(|v| {
            std::env::split_paths(&v)
                .filter(|p| !p.as_os_str().is_empty())
                .collect()
        })
        .unwrap_or_default();
    if let Some(dir) = std::env::current_exe()
        .ok()
        .and_then(|e| e.parent().map(Path::to_path_buf))
    {
        dirs.push(dir.join("plugins"));
    }
    dirs
}

/// An open device. On Windows it may own a VHD attachment, which is
/// detached when this value is dropped (after the device itself).
pub struct Opened {
    pub dev: Arc<dyn BlockDevice>,
    #[cfg(windows)]
    _vhd: Option<winuxfs_win::VhdAttachment>,
}

/// Turns a device-open failure into the message and exit code to show.
pub fn open_error(name: &str, e: Errno) -> CliError {
    match e {
        Errno::EACCES => CliError {
            code: EXIT_PRIVILEGE,
            msg: if cfg!(windows) {
                format!(
                    "Access to {name} was denied: run winuxfs from an elevated \
                     (Run as administrator) prompt, or check the file's permissions."
                )
            } else {
                format!(
                    "Access to {name} was denied: run winuxfs as a user with \
                     permission to read it (for example root)."
                )
            },
        },
        Errno::ENOENT => CliError::usage(format!(
            "Device {name} was not found: check the path or the disk number."
        )),
        Errno::EBUSY => CliError::mount(format!(
            "Device {name} is in use by another program or holds the Windows \
             system volume: close the program, or use --ro to open it read-only."
        )),
        e => CliError::usage(format!(
            "Opening device {name} failed ({}): check that it is a disk, a VHD/VHDX \
             or an image file.",
            errno_text(e)
        )),
    }
}

/// Opens a device argument: on Windows `\\.\PhysicalDriveN`, `disk:N`, a
/// `.vhd`/`.vhdx` file or an image file; elsewhere a file or block device.
pub fn open_device(name: &str, read_only: bool) -> Result<Opened, CliError> {
    open_device_inner(name, read_only).map_err(|e| open_error(name, e))
}

#[cfg(not(windows))]
fn open_device_inner(name: &str, read_only: bool) -> Result<Opened, Errno> {
    let dev = winuxfs_core::FileDevice::open(Path::new(name), read_only)?;
    Ok(Opened { dev: Arc::new(dev) })
}

#[cfg(windows)]
fn open_device_inner(name: &str, read_only: bool) -> Result<Opened, Errno> {
    use winuxfs_win::{attach_vhd, parse_device_spec, DeviceSpec, WinDisk};
    match parse_device_spec(name) {
        DeviceSpec::PhysicalDrive(n) => Ok(Opened {
            dev: WinDisk::open(n, read_only)?,
            _vhd: None,
        }),
        DeviceSpec::Vhd(p) => {
            let a = attach_vhd(&p, read_only)?;
            Ok(Opened {
                dev: a.device(),
                _vhd: Some(a),
            })
        }
        DeviceSpec::Image(p) => Ok(Opened {
            dev: Arc::new(winuxfs_core::FileDevice::open(&p, read_only)?),
            _vhd: None,
        }),
    }
}

/// Reads the partition table of `dev` (named `name` in messages).
pub fn partitions(name: &str, dev: &dyn BlockDevice) -> Result<Vec<PartitionInfo>, CliError> {
    scan(dev).map_err(|e| {
        CliError::usage(format!(
            "Reading the partition table of {name} failed ({e}): check that the \
             device is the one you meant."
        ))
    })
}

/// Formats a GPT type GUID (stored mixed-endian) in its usual text form.
pub fn guid_text(g: &[u8; 16]) -> String {
    format!(
        "{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        u32::from_le_bytes([g[0], g[1], g[2], g[3]]),
        u16::from_le_bytes([g[4], g[5]]),
        u16::from_le_bytes([g[6], g[7]]),
        g[8],
        g[9],
        g[10],
        g[11],
        g[12],
        g[13],
        g[14],
        g[15]
    )
}

/// The device the file system lives on: the whole device when it has no
/// partition table, else partition `partition`, which must then be given.
pub fn select_partition(
    name: &str,
    dev: Arc<dyn BlockDevice>,
    partition: Option<u32>,
) -> Result<Arc<dyn BlockDevice>, CliError> {
    let parts = partitions(name, dev.as_ref())?;
    match (partition, parts.is_empty()) {
        (None, true) => Ok(dev),
        (Some(n), true) => Err(CliError::usage(format!(
            "{name} has no partition table, so there is no partition {n}: \
             leave out --partition to use the whole device."
        ))),
        (None, false) => Err(CliError::usage(format!(
            "{name} has a partition table: choose a partition with --partition N.\n{}",
            crate::cmd_list::partition_table(&parts)
        ))),
        (Some(n), false) => {
            let Some(info) = parts.iter().find(|p| p.index == n) else {
                return Err(CliError::usage(format!(
                    "{name} has no partition {n}: choose one of the partitions below \
                     with --partition N.\n{}",
                    crate::cmd_list::partition_table(&parts)
                )));
            };
            let view = PartitionView::new(dev, info).map_err(|e| {
                CliError::usage(format!(
                    "Partition {n} of {name} cannot be used ({}): check the partition table.",
                    errno_text(e)
                ))
            })?;
            Ok(Arc::new(view))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn guid_text_is_mixed_endian() {
        // Linux filesystem data: 0FC63DAF-8483-4772-8E79-3D69D8477DE4.
        let g = [
            0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47,
            0x7D, 0xE4,
        ];
        assert_eq!(guid_text(&g), "0FC63DAF-8483-4772-8E79-3D69D8477DE4");
    }

    #[test]
    fn explicit_plugin_dirs_replace_defaults() {
        let d = vec![PathBuf::from("a"), PathBuf::from("b")];
        assert_eq!(plugin_dirs(&d), d);
    }

    #[test]
    fn open_errors_choose_exit_codes() {
        assert_eq!(open_error("d", Errno::EACCES).code, EXIT_PRIVILEGE);
        assert_eq!(open_error("d", Errno::ENOENT).code, EXIT_USAGE);
        assert_eq!(open_error("d", Errno::EBUSY).code, EXIT_MOUNT);
        assert_eq!(open_error("d", Errno::EIO).code, EXIT_USAGE);
    }
}
