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
