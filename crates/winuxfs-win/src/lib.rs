// SPDX-License-Identifier: GPL-2.0-only
//! Windows host side of winuxfs.
//!
//! WinFsp is loaded at runtime (see [`WinFsp::load`]) and driven through the
//! hand-written declarations in [`winfsp_ffi`]. The declarations compile on
//! every platform so that their layout tests run on the Linux build host;
//! the loader itself exists only on Windows.
//!
//! [`mount_vfs`] (Windows only) serves a path-level `Vfs` through WinFsp;
//! see the `adapter` module for the name, handle and directory-paging rules.
//!
//! Block devices: [`parse_device_spec`] classifies a device argument on every
//! platform; on Windows `WinDisk` opens physical disks and `attach_vhd`
//! attaches VHD/VHDX files.

pub mod winfsp_ffi;

mod adapter;

pub use adapter::{errno_to_ntstatus, MountOptions};
#[cfg(windows)]
pub use adapter::{mount_vfs, MountedFs};

mod devspec;

pub use devspec::{parse_device_spec, DeviceSpec};

#[cfg(windows)]
mod loader;

#[cfg(windows)]
pub use loader::{winfsp_version, WinFsp};

#[cfg(windows)]
mod vhd;
#[cfg(windows)]
mod windisk;

#[cfg(windows)]
pub use vhd::{attach_vhd, VhdAttachment};
#[cfg(windows)]
pub use windisk::WinDisk;

/// Environment variable naming a WinFsp install directory; when set, it is
/// used instead of the registry (the DLL is `<dir>\bin\winfsp-x64.dll`).
pub const WINFSP_DIR_ENV: &str = "WINUXFS_WINFSP_DIR";

/// Notice that the WinFsp license exception requires in the user interface
/// and user-facing documentation (docs/licensing.md, section 2).
pub const WINFSP_NOTICE: &str =
    "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos \
     <https://github.com/winfsp/winfsp>";

/// Failure to load or query WinFsp.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WinFspError {
    /// No install directory was found, or it has no `bin\winfsp-x64.dll`.
    NotInstalled,
    /// `WINUXFS_WINFSP_DIR` is set to a path that is not absolute.
    InvalidOverride,
    /// The DLL lacks a required export.
    MissingSymbol(&'static str),
    /// A WinFsp call failed with this NTSTATUS.
    Status { what: &'static str, status: i32 },
    /// An argument to a mount call is unusable.
    InvalidArgument(&'static str),
    /// A Win32 call failed with this error code.
    Os { what: &'static str, code: u32 },
}

impl std::fmt::Display for WinFspError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WinFspError::NotInstalled => write!(
                f,
                "WinFsp is not installed (install it from https://winfsp.dev/rel/)"
            ),
            WinFspError::InvalidOverride => {
                write!(f, "{WINFSP_DIR_ENV} must be an absolute path")
            }
            WinFspError::MissingSymbol(s) => write!(f, "winfsp-x64.dll has no export {s}"),
            WinFspError::Status { what, status } => {
                write!(f, "{what} failed: NTSTATUS {:#010x}", *status as u32)
            }
            WinFspError::InvalidArgument(s) => write!(f, "invalid argument: {s}"),
            WinFspError::Os { what, code } => write!(f, "{what} failed: Win32 error {code}"),
        }
    }
}

impl std::error::Error for WinFspError {}
