//! `StdFsPathFs` over a host directory: the shared suite must hold for the
//! build host's own filesystem too (on Windows the e2e tests run it on a
//! mounted winuxfs drive).

use std::io::ErrorKind;
use std::path::PathBuf;
use winuxfs_conformance::*;

fn scratch(tag: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(unique_name(tag));
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[test]
fn run_all_on_host_dir_relative_paths() {
    let root = scratch("stdfs");
    run_all(&StdFsPathFs { root: root.clone() }, "/");
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn rooted_and_relative_paths_join_root() {
    let root = scratch("stdfs-join");
    let fs = StdFsPathFs { root: root.clone() };
    fs.create_dir("/d").unwrap();
    fs.write_file("d/f", 2, b"xy").unwrap();
    assert_eq!(fs.read_file("/d/f", 0, 10).unwrap(), b"\0\0xy");
    assert_eq!(std::fs::read(root.join("d").join("f")).unwrap(), b"\0\0xy");
    assert_eq!(fs.len("/d/f").unwrap(), 4);
    assert!(fs.is_dir("/d").unwrap());
    assert_eq!(fs.list_dir("/d").unwrap(), vec!["f".to_string()]);
    fs.set_len("/d/f", 1).unwrap();
    fs.rename("/d/f", "/d/g").unwrap();
    assert_eq!(fs.read_file("/d/g", 0, 10).unwrap(), b"\0");
    fs.remove_file("/d/g").unwrap();
    fs.remove_dir("/d").unwrap();
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn errors_keep_their_kind() {
    let root = scratch("stdfs-err");
    let fs = StdFsPathFs { root: root.clone() };
    let e = fs.read_file("/missing", 0, 1).unwrap_err();
    assert_eq!(e.kind(), ErrorKind::NotFound);
    fs.create_dir("/d").unwrap();
    fs.write_file("/d/f", 0, b"x").unwrap();
    assert_eq!(
        fs.remove_dir("/d").unwrap_err().kind(),
        ErrorKind::DirectoryNotEmpty
    );
    assert_eq!(
        fs.create_dir("/d").unwrap_err().kind(),
        ErrorKind::AlreadyExists
    );
    std::fs::remove_dir_all(root).unwrap();
}
