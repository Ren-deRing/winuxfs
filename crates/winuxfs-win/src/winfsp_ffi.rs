// SPDX-License-Identifier: GPL-2.0-only
//! Hand-written declarations for the part of the WinFsp user-mode API that
//! winuxfs uses.
//!
//! These are original declarations written from WinFsp's public API
//! documentation (<https://winfsp.dev/doc/>) and the documented binary
//! interface of `winfsp-x64.dll`. No WinFsp header text is included or
//! generated here (see docs/licensing.md, section 3). Type names follow the
//! WinFsp names so that the code can be checked against the documentation;
//! field names are Rust-style.
//!
//! This module has no Windows dependency: the types are plain data and are
//! also compiled on Linux so that the layout tests run on the build host.
//! Only 64-bit targets are supported; `tests/ffi_layout.rs` pins every size.
// The callback table spells out each C signature in place so it can be
// compared with the documentation slot by slot.
#![allow(non_camel_case_types, clippy::type_complexity)]

use core::ffi::c_void;

/// Windows `NTSTATUS`.
pub type NTSTATUS = i32;
/// Windows `BOOLEAN` (one byte, 0 or 1).
pub type BOOLEAN = u8;
/// UTF-16 string pointer (`PWSTR`).
pub type PWSTR = *mut u16;
/// Opaque security descriptor pointer.
pub type PSECURITY_DESCRIPTOR = *mut c_void;
/// Opaque `IO_STATUS_BLOCK` pointer (only passed through).
pub type PIO_STATUS_BLOCK = *mut c_void;
/// Opaque `FILE_FULL_EA_INFORMATION` pointer (only passed through).
pub type PFILE_FULL_EA_INFORMATION = *mut c_void;

/// Device name for disk-type file systems (the `DevicePath` argument of
/// `FspFileSystemCreate`), "WinFsp.Disk".
pub const FSP_FSCTL_DISK_DEVICE_NAME: &str = "WinFsp.Disk";
/// Device name for network-type file systems, "WinFsp.Net".
pub const FSP_FSCTL_NET_DEVICE_NAME: &str = "WinFsp.Net";

/// UNC prefix capacity in UTF-16 units.
pub const VOLUME_PREFIX_CHARS: usize = 192;
/// File system name capacity in UTF-16 units.
pub const VOLUME_FSNAME_CHARS: usize = 16;
/// Volume label capacity in UTF-16 units.
pub const VOLUME_LABEL_CHARS: usize = 32;

/// Size of the original (version 0) volume parameter block. Passing
/// `size_of::<FSP_FSCTL_VOLUME_PARAMS>()` in `version` selects the extended
/// block with the `flags_v1` timeouts.
pub const FSP_FSCTL_VOLUME_PARAMS_V0_SIZE: usize = 456;

/// Volume parameters passed to `FspFileSystemCreate`.
///
/// WinFsp declares the two flag words as C bit fields; they are modelled as
/// plain `u32` words with the bit masks in [`volume_flags`] and
/// [`volume_flags_v1`].
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FSP_FSCTL_VOLUME_PARAMS {
    /// 0, or `size_of::<Self>()` to enable the extended fields.
    pub version: u16,
    pub sector_size: u16,
    pub sectors_per_allocation_unit: u16,
    /// Longest file name component, in bytes.
    pub max_component_length: u16,
    /// Windows FILETIME (100 ns ticks since 1601).
    pub volume_creation_time: u64,
    pub volume_serial_number: u32,
    /// Deprecated by WinFsp; leave 0.
    pub transact_timeout: u32,
    /// Timeout for pending IRPs, milliseconds.
    pub irp_timeout: u32,
    /// Maximum number of pending IRPs.
    pub irp_capacity: u32,
    /// Metadata cache timeout, milliseconds.
    pub file_info_timeout: u32,
    /// See [`volume_flags`].
    pub flags: u32,
    /// UNC prefix for network file systems, NUL-terminated UTF-16.
    pub prefix: [u16; VOLUME_PREFIX_CHARS],
    /// File system name reported to Windows, NUL-terminated UTF-16.
    pub file_system_name: [u16; VOLUME_FSNAME_CHARS],
    // ---- extended part (version == size_of::<Self>()) ----
    /// See [`volume_flags_v1`].
    pub flags_v1: u32,
    pub volume_info_timeout: u32,
    pub dir_info_timeout: u32,
    pub security_timeout: u32,
    pub stream_info_timeout: u32,
    pub ea_timeout: u32,
    pub fsext_control_code: u32,
    pub reserved32: [u32; 1],
    pub reserved64: [u64; 2],
}

impl Default for FSP_FSCTL_VOLUME_PARAMS {
    fn default() -> Self {
        // SAFETY: every field is an integer or an integer array, for which
        // all-zero is a valid value.
        unsafe { core::mem::zeroed() }
    }
}

/// Bits of [`FSP_FSCTL_VOLUME_PARAMS::flags`], in WinFsp's documented order
/// (first listed flag is the least significant bit).
pub mod volume_flags {
    pub const CASE_SENSITIVE_SEARCH: u32 = 1 << 0;
    pub const CASE_PRESERVED_NAMES: u32 = 1 << 1;
    pub const UNICODE_ON_DISK: u32 = 1 << 2;
    pub const PERSISTENT_ACLS: u32 = 1 << 3;
    pub const REPARSE_POINTS: u32 = 1 << 4;
    pub const REPARSE_POINTS_ACCESS_CHECK: u32 = 1 << 5;
    pub const NAMED_STREAMS: u32 = 1 << 6;
    /// Not implemented by WinFsp; keep clear.
    pub const HARD_LINKS: u32 = 1 << 7;
    pub const EXTENDED_ATTRIBUTES: u32 = 1 << 8;
    pub const READ_ONLY_VOLUME: u32 = 1 << 9;
    pub const POST_CLEANUP_WHEN_MODIFIED_ONLY: u32 = 1 << 10;
    pub const PASS_QUERY_DIRECTORY_PATTERN: u32 = 1 << 11;
    pub const ALWAYS_USE_DOUBLE_BUFFERING: u32 = 1 << 12;
    pub const PASS_QUERY_DIRECTORY_FILE_NAME: u32 = 1 << 13;
    pub const FLUSH_AND_PURGE_ON_CLEANUP: u32 = 1 << 14;
    pub const DEVICE_CONTROL: u32 = 1 << 15;
    pub const UM_FILE_CONTEXT_IS_USER_CONTEXT2: u32 = 1 << 16;
    pub const UM_FILE_CONTEXT_IS_FULL_CONTEXT: u32 = 1 << 17;
    pub const UM_NO_REPARSE_POINTS_DIR_CHECK: u32 = 1 << 18;
    // bits 19..=23 are reserved for user mode
    pub const ALLOW_OPEN_IN_KERNEL_MODE: u32 = 1 << 24;
    pub const CASE_PRESERVED_EXTENDED_ATTRIBUTES: u32 = 1 << 25;
    pub const WSL_FEATURES: u32 = 1 << 26;
    pub const DIRECTORY_MARKER_AS_NEXT_OFFSET: u32 = 1 << 27;
    /// Deprecated by WinFsp.
    pub const REJECT_IRP_PRIOR_TO_TRANSACT0: u32 = 1 << 28;
    pub const SUPPORTS_POSIX_UNLINK_RENAME: u32 = 1 << 29;
    pub const POST_DISPOSITION_WHEN_NECESSARY_ONLY: u32 = 1 << 30;
    // bit 31 is reserved for kernel mode
}

/// Bits of [`FSP_FSCTL_VOLUME_PARAMS::flags_v1`]: each marks the matching
/// per-kind timeout field as valid.
pub mod volume_flags_v1 {
    pub const VOLUME_INFO_TIMEOUT_VALID: u32 = 1 << 0;
    pub const DIR_INFO_TIMEOUT_VALID: u32 = 1 << 1;
    pub const SECURITY_TIMEOUT_VALID: u32 = 1 << 2;
    pub const STREAM_INFO_TIMEOUT_VALID: u32 = 1 << 3;
    pub const EA_TIMEOUT_VALID: u32 = 1 << 4;
}

/// Volume size and label, filled in by `GetVolumeInfo`/`SetVolumeLabel`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FSP_FSCTL_VOLUME_INFO {
    pub total_size: u64,
    pub free_size: u64,
    /// Label length in bytes (not characters).
    pub volume_label_length: u16,
    pub volume_label: [u16; VOLUME_LABEL_CHARS],
}

/// Per-file metadata returned by most callbacks.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct FSP_FSCTL_FILE_INFO {
    /// `FILE_ATTRIBUTE_*` bits.
    pub file_attributes: u32,
    pub reparse_tag: u32,
    pub allocation_size: u64,
    pub file_size: u64,
    /// Timestamps are Windows FILETIME values.
    pub creation_time: u64,
    pub last_access_time: u64,
    pub last_write_time: u64,
    pub change_time: u64,
    /// Stable file id (inode number).
    pub index_number: u64,
    /// Not implemented by WinFsp; keep 0.
    pub hard_links: u32,
    pub ea_size: u32,
}

/// The 24-byte tail of [`FSP_FSCTL_DIR_INFO`]. WinFsp sizes it like the
/// matching NT directory record; only `next_offset` carries meaning.
#[repr(C)]
#[derive(Clone, Copy)]
pub union FSP_FSCTL_DIR_INFO_TAIL {
    pub next_offset: u64,
    pub padding: [u8; 24],
}

/// A directory entry for `FspFileSystemAddDirInfo`. The UTF-16 name (not
/// NUL-terminated) follows the fixed part; `size` covers both, in bytes.
#[repr(C)]
pub struct FSP_FSCTL_DIR_INFO {
    pub size: u16,
    pub file_info: FSP_FSCTL_FILE_INFO,
    pub u: FSP_FSCTL_DIR_INFO_TAIL,
    /// Start of the variable-length name.
    pub file_name_buf: [u16; 0],
}

/// The file system object WinFsp allocates in `FspFileSystemCreate`.
///
/// Only the leading fields that winuxfs touches are declared; everything
/// after `user_context` is private to WinFsp. Never create or copy one of
/// these: use the pointer WinFsp hands out.
#[repr(C)]
pub struct FSP_FILE_SYSTEM {
    pub version: u16,
    /// Free for the file system's own use (winuxfs stores its context here).
    pub user_context: *mut c_void,
    _private: [u8; 0],
}

type FsPtr = *mut FSP_FILE_SYSTEM;

/// The callback table. WinFsp calls through these from its dispatcher
/// threads; `None` means "not supported". The table is fixed at 64 slots.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FSP_FILE_SYSTEM_INTERFACE {
    pub get_volume_info:
        Option<unsafe extern "system" fn(FsPtr, *mut FSP_FSCTL_VOLUME_INFO) -> NTSTATUS>,
    pub set_volume_label:
        Option<unsafe extern "system" fn(FsPtr, PWSTR, *mut FSP_FSCTL_VOLUME_INFO) -> NTSTATUS>,
    /// (fs, name, attributes-or-reparse-index out, sd buffer, sd size in/out)
    pub get_security_by_name: Option<
        unsafe extern "system" fn(
            FsPtr,
            PWSTR,
            *mut u32,
            PSECURITY_DESCRIPTOR,
            *mut usize,
        ) -> NTSTATUS,
    >,
    /// (fs, name, create options, granted access, attributes, sd,
    /// allocation size, file context out, file info out)
    pub create: Option<
        unsafe extern "system" fn(
            FsPtr,
            PWSTR,
            u32,
            u32,
            u32,
            PSECURITY_DESCRIPTOR,
            u64,
            *mut *mut c_void,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// (fs, name, create options, granted access, file context out, info out)
    pub open: Option<
        unsafe extern "system" fn(
            FsPtr,
            PWSTR,
            u32,
            u32,
            *mut *mut c_void,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// (fs, ctx, attributes, replace attributes, allocation size, info out)
    pub overwrite: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            u32,
            BOOLEAN,
            u64,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// (fs, ctx, name, cleanup flags)
    pub cleanup: Option<unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, u32)>,
    pub close: Option<unsafe extern "system" fn(FsPtr, *mut c_void)>,
    /// (fs, ctx, buffer, offset, length, bytes transferred out)
    pub read: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, *mut c_void, u64, u32, *mut u32) -> NTSTATUS,
    >,
    /// (fs, ctx, buffer, offset, length, write to end, constrained io,
    /// bytes transferred out, info out)
    pub write: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            *mut c_void,
            u64,
            u32,
            BOOLEAN,
            BOOLEAN,
            *mut u32,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// ctx is null when the whole volume is flushed.
    pub flush:
        Option<unsafe extern "system" fn(FsPtr, *mut c_void, *mut FSP_FSCTL_FILE_INFO) -> NTSTATUS>,
    pub get_file_info:
        Option<unsafe extern "system" fn(FsPtr, *mut c_void, *mut FSP_FSCTL_FILE_INFO) -> NTSTATUS>,
    /// (fs, ctx, attributes, creation, access, write, change, info out)
    pub set_basic_info: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            u32,
            u64,
            u64,
            u64,
            u64,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// (fs, ctx, new size, is allocation size, info out)
    pub set_file_size: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            u64,
            BOOLEAN,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    pub can_delete: Option<unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR) -> NTSTATUS>,
    /// (fs, ctx, old name, new name, replace if exists)
    pub rename:
        Option<unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, PWSTR, BOOLEAN) -> NTSTATUS>,
    pub get_security: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, PSECURITY_DESCRIPTOR, *mut usize) -> NTSTATUS,
    >,
    /// (fs, ctx, SECURITY_INFORMATION bits, modification descriptor)
    pub set_security: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, u32, PSECURITY_DESCRIPTOR) -> NTSTATUS,
    >,
    /// (fs, ctx, pattern, marker, buffer, length, bytes transferred out)
    pub read_directory: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            PWSTR,
            PWSTR,
            *mut c_void,
            u32,
            *mut u32,
        ) -> NTSTATUS,
    >,
    /// (fs, name, reparse index, resolve last component, io status,
    /// buffer, size in/out)
    pub resolve_reparse_points: Option<
        unsafe extern "system" fn(
            FsPtr,
            PWSTR,
            u32,
            BOOLEAN,
            PIO_STATUS_BLOCK,
            *mut c_void,
            *mut usize,
        ) -> NTSTATUS,
    >,
    pub get_reparse_point: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, *mut c_void, *mut usize) -> NTSTATUS,
    >,
    pub set_reparse_point: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, *mut c_void, usize) -> NTSTATUS,
    >,
    pub delete_reparse_point: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, *mut c_void, usize) -> NTSTATUS,
    >,
    /// (fs, ctx, buffer, length, bytes transferred out)
    pub get_stream_info: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, *mut c_void, u32, *mut u32) -> NTSTATUS,
    >,
    pub get_dir_info_by_name: Option<
        unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, *mut FSP_FSCTL_DIR_INFO) -> NTSTATUS,
    >,
    /// (fs, ctx, control code, in buf, in len, out buf, out len, bytes out)
    pub control: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            u32,
            *mut c_void,
            u32,
            *mut c_void,
            u32,
            *mut u32,
        ) -> NTSTATUS,
    >,
    pub set_delete:
        Option<unsafe extern "system" fn(FsPtr, *mut c_void, PWSTR, BOOLEAN) -> NTSTATUS>,
    /// `create` plus (extra buffer, extra length, extra is reparse point)
    /// before the context/info outputs.
    pub create_ex: Option<
        unsafe extern "system" fn(
            FsPtr,
            PWSTR,
            u32,
            u32,
            u32,
            PSECURITY_DESCRIPTOR,
            u64,
            *mut c_void,
            u32,
            BOOLEAN,
            *mut *mut c_void,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// `overwrite` plus (EA buffer, EA length) before the info output.
    pub overwrite_ex: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            u32,
            BOOLEAN,
            u64,
            PFILE_FULL_EA_INFORMATION,
            u32,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    pub get_ea: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            PFILE_FULL_EA_INFORMATION,
            u32,
            *mut u32,
        ) -> NTSTATUS,
    >,
    pub set_ea: Option<
        unsafe extern "system" fn(
            FsPtr,
            *mut c_void,
            PFILE_FULL_EA_INFORMATION,
            u32,
            *mut FSP_FSCTL_FILE_INFO,
        ) -> NTSTATUS,
    >,
    /// Slot kept for compatibility; WinFsp no longer calls it.
    pub obsolete0: Option<unsafe extern "system" fn() -> NTSTATUS>,
    /// (fs, stopped normally)
    pub dispatcher_stopped: Option<unsafe extern "system" fn(FsPtr, BOOLEAN)>,
    /// Unused slots; must stay `None`.
    pub reserved: [Option<unsafe extern "system" fn() -> NTSTATUS>; 31],
}

// ---- exported DLL functions (resolved at runtime with GetProcAddress) ----

/// `FspFileSystemCreate(device path, volume params, interface, fs out)`.
pub type FspFileSystemCreateFn = unsafe extern "system" fn(
    PWSTR,
    *const FSP_FSCTL_VOLUME_PARAMS,
    *const FSP_FILE_SYSTEM_INTERFACE,
    *mut *mut FSP_FILE_SYSTEM,
) -> NTSTATUS;
/// `FspFileSystemDelete(fs)`.
pub type FspFileSystemDeleteFn = unsafe extern "system" fn(*mut FSP_FILE_SYSTEM);
/// `FspFileSystemSetMountPoint(fs, mount point or null for any drive)`.
pub type FspFileSystemSetMountPointFn =
    unsafe extern "system" fn(*mut FSP_FILE_SYSTEM, PWSTR) -> NTSTATUS;
/// `FspFileSystemRemoveMountPoint(fs)`.
pub type FspFileSystemRemoveMountPointFn = unsafe extern "system" fn(*mut FSP_FILE_SYSTEM);
/// `FspFileSystemStartDispatcher(fs, thread count or 0 for default)`.
pub type FspFileSystemStartDispatcherFn =
    unsafe extern "system" fn(*mut FSP_FILE_SYSTEM, u32) -> NTSTATUS;
/// `FspFileSystemStopDispatcher(fs)`.
pub type FspFileSystemStopDispatcherFn = unsafe extern "system" fn(*mut FSP_FILE_SYSTEM);
/// `FspFileSystemAddDirInfo(entry or null for end, buffer, length, bytes
/// transferred in/out)`; returns false when the buffer is full.
pub type FspFileSystemAddDirInfoFn =
    unsafe extern "system" fn(*mut FSP_FSCTL_DIR_INFO, *mut c_void, u32, *mut u32) -> BOOLEAN;
