use std::mem::size_of;
use winuxfs_abi::*;

#[test]
fn abi_version_is_1() {
    assert_eq!(WFS_ABI_VERSION, 1);
}

#[test]
fn struct_sizes() {
    assert_eq!(size_of::<wfs_time>(), 16);
    // 120, not 144: ino 8 + ftype/mode 8 + five u32 20 = 36, pad to 40,
    // size+blocks 16 = 56, four wfs_time (16 each) = 64 -> 120.
    assert_eq!(size_of::<wfs_stat>(), 120);
    assert_eq!(size_of::<wfs_dirent>(), 32);
}

// Pointer-sized fields: the sizes below are the 64-bit ones (all targets this
// project builds for).
#[cfg(target_pointer_width = "64")]
#[test]
fn statfs_and_host_api_sizes() {
    // 5 x u64 + 2 x u32 = 48.
    assert_eq!(size_of::<wfs_statfs>(), 48);
    // abi_version + size (8) + 7 function pointers (56) = 64.
    assert_eq!(size_of::<wfs_host_api>(), 64);
}

#[test]
fn contract_constants() {
    assert_eq!(WFS_ROOT_INO, 1);
    assert_eq!(WFS_NAME_MAX, 255);
}
