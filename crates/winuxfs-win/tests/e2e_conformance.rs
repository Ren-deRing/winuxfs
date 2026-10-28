// SPDX-License-Identifier: GPL-2.0-only
//! The shared conformance suite (`winuxfs_conformance::run_all`, the same
//! bodies the Vfs runs in `vfs_conformance.rs`) through `std::fs` on a
//! mounted winuxfs drive (Windows only; run in the test VM).
#![cfg(windows)]

mod common;

use std::path::PathBuf;

use common::*;
use winuxfs_conformance::{
    conf_case_insensitive_collision, conf_case_sensitive_coexist, join, run_all, unique_name,
    PathFs, StdFsPathFs,
};

fn drive_fs() -> StdFsPathFs {
    StdFsPathFs {
        root: PathBuf::from(format!("{DRIVE}\\")),
    }
}

#[test]
fn run_all_on_mounted_drive() {
    let _m = mount_empty();
    run_all(&drive_fs(), &format!("{DRIVE}\\"));
}

#[test]
fn case_sensitive_coexist_on_drive() {
    let _m = mount_empty();
    let fs = drive_fs();
    let dir = join(&format!("{DRIVE}\\"), &unique_name("cs"));
    fs.create_dir(&dir).unwrap();
    conf_case_sensitive_coexist(&fs, &dir);
}

#[test]
fn case_insensitive_collision_on_drive() {
    let _m = mount_with(
        Setup {
            case_insensitive: true,
            read_only: false,
        },
        |_| {},
    );
    let fs = drive_fs();
    let dir = join(&format!("{DRIVE}\\"), &unique_name("ci"));
    fs.create_dir(&dir).unwrap();
    conf_case_insensitive_collision(&fs, &dir);
}
