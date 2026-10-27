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
