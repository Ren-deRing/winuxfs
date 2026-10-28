// SPDX-License-Identifier: GPL-2.0-only
//! Runtime loading of `winfsp-x64.dll`.
//!
//! winuxfs never links against WinFsp's import library: the DLL is found
//! through the WinFsp install directory and loaded by its absolute path with
//! `LoadLibraryExW` (restricted search: the DLL's own directory and the
//! default safe directories, never the current directory or `PATH`), and
//! each function is looked up with `GetProcAddress` (docs/licensing.md,
//! section 3). WinFsp itself is not bundled; the user installs it.
//!
//! The function pointers are private: they are only reachable through
//! methods on [`WinFsp`], whose clones share one reference-counted module,
//! so no pointer can outlive the `FreeLibrary` call.

use std::ffi::OsString;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use windows_sys::Win32::Foundation::{FreeLibrary, GetLastError, HMODULE};
use windows_sys::Win32::Storage::FileSystem::{
    GetFileVersionInfoSizeW, GetFileVersionInfoW, VerQueryValueW, VS_FIXEDFILEINFO,
};
use windows_sys::Win32::System::LibraryLoader::{
    GetProcAddress, LoadLibraryExW, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
};
use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ};

use crate::winfsp_ffi::*;
use crate::{WinFspError, WINFSP_DIR_ENV};

/// WinFsp's installer is 32-bit, so its key lives in the WOW64 view.
const INSTALL_KEY: &str = r"SOFTWARE\WOW6432Node\WinFsp";
const INSTALL_VALUE: &str = "InstallDir";
const DLL_NAME: &str = "winfsp-x64.dll";

/// NUL-terminated UTF-16 copy of `s` for Win32 calls.
fn wide(s: impl AsRef<std::ffi::OsStr>) -> Vec<u16> {
    s.as_ref().encode_wide().chain(Some(0)).collect()
}

fn last_error(what: &'static str) -> WinFspError {
    // SAFETY: GetLastError has no preconditions.
    let code = unsafe { GetLastError() };
    WinFspError::Os { what, code }
}

/// Reads `HKLM\SOFTWARE\WOW6432Node\WinFsp\InstallDir`; `None` when the key
/// or value is missing or unreadable.
fn registry_install_dir() -> Option<PathBuf> {
    let key = wide(INSTALL_KEY);
    let value = wide(INSTALL_VALUE);
    let mut buf: Vec<u16> = vec![0; 260];
    for _ in 0..2 {
        let mut bytes = (buf.len() * 2) as u32;
        // SAFETY: key/value are NUL-terminated; buf has `bytes` writable
        // bytes and RegGetValueW never writes more than that.
        let rc = unsafe {
            RegGetValueW(
                HKEY_LOCAL_MACHINE,
                key.as_ptr(),
                value.as_ptr(),
                RRF_RT_REG_SZ,
                std::ptr::null_mut(),
                buf.as_mut_ptr().cast(),
                &mut bytes,
            )
        };
        match rc {
            0 => {
                let chars = (bytes as usize / 2).min(buf.len());
                let s = &buf[..chars];
                let end = s.iter().position(|&c| c == 0).unwrap_or(s.len());
                return Some(PathBuf::from(OsString::from_wide(&s[..end])));
            }
            // ERROR_MORE_DATA: `bytes` now holds the required size.
            234 => buf = vec![0; (bytes as usize).div_ceil(2) + 1],
            _ => return None,
        }
    }
    None
}

/// Finds `<InstallDir>\bin\winfsp-x64.dll`. `WINUXFS_WINFSP_DIR`, when set,
/// replaces the registry lookup (used by tests and unusual installs); it
/// must be an absolute path (`C:\...` or `\\server\share\...`), otherwise
/// [`WinFspError::InvalidOverride`]. A registry value that is not absolute
/// is treated as "not installed".
pub(crate) fn locate_dll() -> Result<PathBuf, WinFspError> {
    let dir = match std::env::var_os(WINFSP_DIR_ENV) {
        Some(d) if !d.is_empty() => {
            let d = PathBuf::from(d);
            if !d.is_absolute() {
                return Err(WinFspError::InvalidOverride);
            }
            d
        }
        _ => registry_install_dir()
            .filter(|d| d.is_absolute())
            .ok_or(WinFspError::NotInstalled)?,
    };
    let dll = dir.join("bin").join(DLL_NAME);
    if dll.is_file() {
        Ok(dll)
    } else {
        Err(WinFspError::NotInstalled)
    }
}

/// A loaded `winfsp-x64.dll` and the entry points winuxfs uses.
///
/// Cloning is cheap and shares the module; the DLL is released when the
/// last clone is dropped. Anything that may still call into WinFsp (a
/// running dispatcher, a live `FSP_FILE_SYSTEM`) must hold a clone, which
/// is how the WinFsp adapter keeps the DLL loaded until it is torn down.
#[derive(Clone)]
pub struct WinFsp {
    inner: Arc<Loaded>,
}

struct Loaded {
    module: HMODULE,
    dll_path: PathBuf,
    file_system_create: FspFileSystemCreateFn,
    file_system_delete: FspFileSystemDeleteFn,
    file_system_set_mount_point: FspFileSystemSetMountPointFn,
    file_system_remove_mount_point: FspFileSystemRemoveMountPointFn,
    file_system_start_dispatcher: FspFileSystemStartDispatcherFn,
    file_system_stop_dispatcher: FspFileSystemStopDispatcherFn,
    file_system_add_dir_info: FspFileSystemAddDirInfoFn,
}

// SAFETY: a module handle and plain function pointers into a loaded DLL may
// be used from any thread; WinFsp's API is thread-safe for these calls.
unsafe impl Send for Loaded {}
unsafe impl Sync for Loaded {}

impl Drop for Loaded {
    fn drop(&mut self) {
        // SAFETY: module came from LoadLibraryExW; the fn pointers are only
        // reachable through this value, which is going away.
        unsafe { FreeLibrary(self.module) };
    }
}

impl std::fmt::Debug for WinFsp {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("WinFsp")
            .field("dll_path", &self.inner.dll_path)
            .finish_non_exhaustive()
    }
}

/// Looks up `name` and reinterprets it as the function pointer type `F`.
///
/// # Safety
/// `F` must be a function pointer type matching the export's real signature.
unsafe fn resolve<F: Copy>(module: HMODULE, name: &'static str) -> Result<F, WinFspError> {
    const { assert!(std::mem::size_of::<F>() == std::mem::size_of::<unsafe extern "system" fn()>()) };
    let cname: Vec<u8> = name.bytes().chain(Some(0)).collect();
    // SAFETY: module is a live handle and cname is NUL-terminated.
    let proc = unsafe { GetProcAddress(module, cname.as_ptr()) };
    match proc {
        // SAFETY: same size (checked above); the caller vouches for the type.
        Some(p) => Ok(unsafe { std::mem::transmute_copy(&p) }),
        None => Err(WinFspError::MissingSymbol(name)),
    }
}

impl WinFsp {
    /// Locates and loads the installed WinFsp DLL and resolves every entry
    /// point. Fails with [`WinFspError::NotInstalled`] when WinFsp is not
    /// found, [`WinFspError::InvalidOverride`] when `WINUXFS_WINFSP_DIR` is
    /// not absolute and [`WinFspError::MissingSymbol`] when the DLL lacks a
    /// function.
    pub fn load() -> Result<WinFsp, WinFspError> {
        let dll_path = locate_dll()?;
        let wpath = wide(&dll_path);
        // SAFETY: wpath is a NUL-terminated absolute path; the flags limit
        // the DLL's own dependencies to its directory and the safe defaults.
        let module = unsafe {
            LoadLibraryExW(
                wpath.as_ptr(),
                std::ptr::null_mut(),
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
            )
        };
        if module.is_null() {
            return Err(last_error("LoadLibraryExW"));
        }
        // SAFETY: each type alias in winfsp_ffi matches the documented
        // signature of the export with the same name.
        let resolved = unsafe {
            (|| {
                Ok::<_, WinFspError>(Loaded {
                    module,
                    dll_path: dll_path.clone(),
                    file_system_create: resolve(module, "FspFileSystemCreate")?,
                    file_system_delete: resolve(module, "FspFileSystemDelete")?,
                    file_system_set_mount_point: resolve(module, "FspFileSystemSetMountPoint")?,
                    file_system_remove_mount_point: resolve(
                        module,
                        "FspFileSystemRemoveMountPoint",
                    )?,
                    file_system_start_dispatcher: resolve(module, "FspFileSystemStartDispatcher")?,
                    file_system_stop_dispatcher: resolve(module, "FspFileSystemStopDispatcher")?,
                    file_system_add_dir_info: resolve(module, "FspFileSystemAddDirInfo")?,
                })
            })()
        };
        match resolved {
            Ok(inner) => Ok(WinFsp {
                inner: Arc::new(inner),
            }),
            Err(e) => {
                // SAFETY: module came from LoadLibraryExW and nothing from it
                // is retained on this path.
                unsafe { FreeLibrary(module) };
                Err(e)
            }
        }
    }

    /// Full path of the loaded DLL.
    pub fn dll_path(&self) -> &Path {
        &self.inner.dll_path
    }

    /// `FspFileSystemCreate`.
    ///
    /// # Safety
    /// `device` is a NUL-terminated UTF-16 string; `params` and `out` are
    /// valid. `interface` must stay valid and unchanged until the file
    /// system is deleted: WinFsp keeps the pointer, it does not copy the
    /// table.
    pub unsafe fn file_system_create(
        &self,
        device: PWSTR,
        params: *const FSP_FSCTL_VOLUME_PARAMS,
        interface: *const FSP_FILE_SYSTEM_INTERFACE,
        out: *mut *mut FSP_FILE_SYSTEM,
    ) -> NTSTATUS {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_create)(device, params, interface, out) }
    }

    /// `FspFileSystemDelete` (also removes the mount point).
    ///
    /// # Safety
    /// `fs` came from [`Self::file_system_create`] and is not used again.
    pub unsafe fn file_system_delete(&self, fs: *mut FSP_FILE_SYSTEM) {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_delete)(fs) }
    }

    /// `FspFileSystemSetMountPoint`.
    ///
    /// # Safety
    /// `fs` is live; `mount_point` is NUL-terminated UTF-16 or null.
    pub unsafe fn file_system_set_mount_point(
        &self,
        fs: *mut FSP_FILE_SYSTEM,
        mount_point: PWSTR,
    ) -> NTSTATUS {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_set_mount_point)(fs, mount_point) }
    }

    /// `FspFileSystemRemoveMountPoint` (a no-op when none is set).
    ///
    /// # Safety
    /// `fs` is live.
    pub unsafe fn file_system_remove_mount_point(&self, fs: *mut FSP_FILE_SYSTEM) {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_remove_mount_point)(fs) }
    }

    /// `FspFileSystemStartDispatcher` (0 threads = WinFsp's default).
    ///
    /// # Safety
    /// `fs` is live and its callbacks are ready to be called.
    pub unsafe fn file_system_start_dispatcher(
        &self,
        fs: *mut FSP_FILE_SYSTEM,
        threads: u32,
    ) -> NTSTATUS {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_start_dispatcher)(fs, threads) }
    }

    /// `FspFileSystemStopDispatcher`: returns after every dispatcher thread
    /// (and so every running callback) has finished.
    ///
    /// # Safety
    /// `fs` is live; not called from a dispatcher thread.
    pub unsafe fn file_system_stop_dispatcher(&self, fs: *mut FSP_FILE_SYSTEM) {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_stop_dispatcher)(fs) }
    }

    /// `FspFileSystemAddDirInfo`: appends `entry` (or the end marker when
    /// null) to `buf` at offset `*transferred`; false when it does not fit.
    ///
    /// # Safety
    /// `buf` has `len` writable bytes; `entry` is null or a complete entry
    /// whose `size` covers its name; `transferred` is valid.
    pub unsafe fn add_dir_info(
        &self,
        entry: *mut FSP_FSCTL_DIR_INFO,
        buf: *mut core::ffi::c_void,
        len: u32,
        transferred: *mut u32,
    ) -> bool {
        // SAFETY: forwarded to the caller.
        unsafe { (self.inner.file_system_add_dir_info)(entry, buf, len, transferred) != 0 }
    }
}

/// Major and minor file version of the installed `winfsp-x64.dll`, read
/// from its version resource (the DLL is not loaded for this).
pub fn winfsp_version() -> Result<(u16, u16), WinFspError> {
    let path = wide(locate_dll()?);
    let mut handle = 0u32;
    // SAFETY: path is NUL-terminated; handle is a valid out pointer.
    let len = unsafe { GetFileVersionInfoSizeW(path.as_ptr(), &mut handle) };
    if len == 0 {
        return Err(last_error("GetFileVersionInfoSizeW"));
    }
    let mut data = vec![0u8; len as usize];
    // SAFETY: data has `len` writable bytes.
    if unsafe { GetFileVersionInfoW(path.as_ptr(), 0, len, data.as_mut_ptr().cast()) } == 0 {
        return Err(last_error("GetFileVersionInfoW"));
    }
    let root = wide("\\");
    let mut info: *mut std::ffi::c_void = std::ptr::null_mut();
    let mut info_len = 0u32;
    // SAFETY: data holds a version block from GetFileVersionInfoW; the
    // returned pointer points into it and is used while data lives.
    let ok = unsafe {
        VerQueryValueW(
            data.as_ptr().cast(),
            root.as_ptr(),
            &mut info,
            &mut info_len,
        )
    };
    if ok == 0 || info.is_null() || (info_len as usize) < std::mem::size_of::<VS_FIXEDFILEINFO>() {
        return Err(last_error("VerQueryValueW"));
    }
    // SAFETY: checked non-null and large enough above; read_unaligned
    // because the block is a byte buffer.
    let fixed = unsafe { std::ptr::read_unaligned(info as *const VS_FIXEDFILEINFO) };
    Ok((
        (fixed.dwFileVersionMS >> 16) as u16,
        (fixed.dwFileVersionMS & 0xffff) as u16,
    ))
}
