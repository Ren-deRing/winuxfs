// SPDX-License-Identifier: GPL-2.0-only
//! Windows host side of winuxfs.
//!
//! WinFsp is loaded at runtime (see [`WinFsp::load`]) and driven through the
//! hand-written declarations in [`winfsp_ffi`]. The declarations compile on
//! every platform so that their layout tests run on the Linux build host;
//! the loader itself exists only on Windows.

pub mod winfsp_ffi;

#[cfg(windows)]
mod loader;

#[cfg(windows)]
pub use loader::{winfsp_version, WinFsp};

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
    /// The DLL lacks a required export.
    MissingSymbol(&'static str),
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
            WinFspError::MissingSymbol(s) => write!(f, "winfsp-x64.dll has no export {s}"),
            WinFspError::Os { what, code } => write!(f, "{what} failed: Win32 error {code}"),
        }
    }
}

impl std::error::Error for WinFspError {}
