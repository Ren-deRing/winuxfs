// SPDX-License-Identifier: GPL-2.0-only
//! Layout checks for the hand-written WinFsp declarations.
//!
//! A wrong layout silently corrupts memory, so every struct is checked
//! against the size WinFsp itself guarantees (the public headers carry a
//! static size assertion for each of these types) and against offsets
//! computed by hand from the documented field list. The numbers are for
//! 64-bit targets (x86_64 Windows, and x86_64 Linux where u64 and pointers
//! have the same size and alignment), which is all winuxfs supports.
#![cfg(target_pointer_width = "64")]

use std::mem::{align_of, offset_of, size_of};

use winuxfs_win::winfsp_ffi::*;

#[test]
fn volume_params_layout() {
    // u16 x4 (0..8), u64 creation time (8), u32 serial (16), four u32
    // timeouts/capacity (20..36), u32 flag word (36), 192 WCHAR prefix
    // (40..424), 16 WCHAR fs name (424..456) = 456 for the V0 part. V1
    // adds a u32 flag word (456), six u32 values (460..484), one reserved
    // u32 (484), then two u64 at 488 = 504.
    assert_eq!(size_of::<FSP_FSCTL_VOLUME_PARAMS>(), 504);
    assert_eq!(align_of::<FSP_FSCTL_VOLUME_PARAMS>(), 8);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, volume_creation_time), 8);
    assert_eq!(
        offset_of!(FSP_FSCTL_VOLUME_PARAMS, volume_serial_number),
        16
    );
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, file_info_timeout), 32);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, flags), 36);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, prefix), 40);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, file_system_name), 424);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, flags_v1), 456);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, fsext_control_code), 480);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_PARAMS, reserved64), 488);
    assert_eq!(FSP_FSCTL_VOLUME_PARAMS_V0_SIZE, 456);
}

#[test]
fn volume_info_layout() {
    // u64 + u64 + u16 + 32 WCHAR = 82, padded to the u64 alignment = 88.
    assert_eq!(size_of::<FSP_FSCTL_VOLUME_INFO>(), 88);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_INFO, volume_label_length), 16);
    assert_eq!(offset_of!(FSP_FSCTL_VOLUME_INFO, volume_label), 18);
}

#[test]
fn file_info_layout() {
    // u32 + u32 + seven u64 + u32 + u32 = 8 + 56 + 8 = 72.
    assert_eq!(size_of::<FSP_FSCTL_FILE_INFO>(), 72);
    assert_eq!(offset_of!(FSP_FSCTL_FILE_INFO, allocation_size), 8);
    assert_eq!(offset_of!(FSP_FSCTL_FILE_INFO, index_number), 56);
    assert_eq!(offset_of!(FSP_FSCTL_FILE_INFO, hard_links), 64);
    assert_eq!(offset_of!(FSP_FSCTL_FILE_INFO, ea_size), 68);
}

#[test]
fn dir_info_layout() {
    // u16 size, padding to 8, 72-byte file info (8..80), 24-byte union
    // (80..104), then the variable-length name = 104 fixed bytes.
    assert_eq!(size_of::<FSP_FSCTL_DIR_INFO>(), 104);
    assert_eq!(offset_of!(FSP_FSCTL_DIR_INFO, file_info), 8);
    assert_eq!(offset_of!(FSP_FSCTL_DIR_INFO, u), 80);
    assert_eq!(offset_of!(FSP_FSCTL_DIR_INFO, file_name_buf), 104);
}

#[test]
fn interface_layout() {
    // 33 named callbacks + 31 reserved slots = 64 pointers of 8 bytes.
    assert_eq!(size_of::<FSP_FILE_SYSTEM_INTERFACE>(), 64 * 8);
    assert_eq!(offset_of!(FSP_FILE_SYSTEM_INTERFACE, get_volume_info), 0);
    assert_eq!(
        offset_of!(FSP_FILE_SYSTEM_INTERFACE, read_directory),
        18 * 8
    );
    assert_eq!(
        offset_of!(FSP_FILE_SYSTEM_INTERFACE, dispatcher_stopped),
        32 * 8
    );
    assert_eq!(offset_of!(FSP_FILE_SYSTEM_INTERFACE, reserved), 33 * 8);
}

#[test]
fn file_system_head_layout() {
    // The file system object starts with a u16 version, then the user
    // context pointer at the next pointer-aligned offset.
    assert_eq!(offset_of!(FSP_FILE_SYSTEM, user_context), 8);
}

#[test]
fn volume_params_flag_bits() {
    // Bit fields are allocated from the least significant bit in declaration
    // order (MSVC and the mingw ABI agree for u32 bit fields).
    assert_eq!(volume_flags::CASE_SENSITIVE_SEARCH, 1 << 0);
    assert_eq!(volume_flags::READ_ONLY_VOLUME, 1 << 9);
    assert_eq!(volume_flags::POST_CLEANUP_WHEN_MODIFIED_ONLY, 1 << 10);
    assert_eq!(volume_flags::UM_FILE_CONTEXT_IS_USER_CONTEXT2, 1 << 16);
    assert_eq!(volume_flags::ALLOW_OPEN_IN_KERNEL_MODE, 1 << 24);
    assert_eq!(volume_flags::POST_DISPOSITION_WHEN_NECESSARY_ONLY, 1 << 30);
    assert_eq!(volume_flags_v1::EA_TIMEOUT_VALID, 1 << 4);
}
