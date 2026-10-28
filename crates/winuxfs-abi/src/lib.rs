//! Backend plugin ABI v1 (bindgen-generated from include/winuxfs/backend.h).
#![allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code
)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

// WFS_ABI_VERSION (u32) is emitted by bindgen from the header macro.

// Layout pins that must hold at compile time, not only in `cargo test`.
#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(std::mem::size_of::<wfs_statfs>() == 48);
    assert!(std::mem::size_of::<wfs_host_api>() == 64);
};
