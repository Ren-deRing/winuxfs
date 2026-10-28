mod common;

use common::*;
use std::sync::Arc;
use winuxfs_abi as abi;
use winuxfs_core::hostapi::{host_api, BlkHandle};
use winuxfs_core::plugin::*;
use winuxfs_core::vfs::{Vfs, VfsOptions};
use winuxfs_core::*;

fn testplug() -> std::path::PathBuf {
    test_plugin("testplug", "testplug")
}

fn load_err(p: &std::path::Path) -> PluginError {
    match load_plugin(p) {
        Ok(_) => panic!("{} unexpectedly loaded", p.display()),
        Err(e) => e,
    }
}

fn no_devs() -> Devices {
    Devices {
        data: None,
        log: None,
        rt: None,
    }
}

/// Opens the test plugin at `p` (same library instance the loader gets) so
/// its C hooks can be read. Holding it keeps the instance and its globals alive.
fn open_hooks(p: &std::path::Path) -> libloading::Library {
    unsafe { libloading::Library::new(p).unwrap() }
}

/// Reads a C `int f(void)` hook from an opened test plugin.
fn hook(lib: &libloading::Library, sym: &[u8]) -> i32 {
    unsafe {
        let f: libloading::Symbol<unsafe extern "C" fn() -> i32> = lib.get(sym).unwrap();
        f()
    }
}

// ---- loader errors ----

#[test]
fn load_missing_file_is_load_error() {
    let e = load_err(std::path::Path::new("/nonexistent/winuxfs_backend_nope.so"));
    assert!(matches!(e, PluginError::Load(_)), "{e:?}");
}

#[test]
fn abi_mismatch_reported() {
    let e = load_err(&test_plugin("bad", "bad_abi"));
    assert_eq!(
        e,
        PluginError::AbiMismatch {
            plugin: 99,
            host: 1
        }
    );
}

#[test]
fn missing_mandatory_op_is_invalid() {
    let e = load_err(&test_plugin("bad", "bad_noops"));
    assert!(matches!(e, PluginError::Invalid(_)), "{e:?}");
}

#[test]
fn missing_entry_symbol() {
    let e = load_err(&test_plugin("bad", "bad_noentry"));
    assert_eq!(e, PluginError::MissingEntry);
}

#[test]
fn discover_skips_non_plugins_and_reports_errors() {
    let d = scratch_dir("discover");
    for n in ["bad_abi", "bad_noops", "bad_noentry"] {
        let src = test_plugin("bad", n);
        std::fs::copy(&src, d.join(src.file_name().unwrap())).unwrap();
    }
    std::fs::write(d.join("readme.txt"), "not a plugin").unwrap();
    std::fs::write(d.join(format!("other_lib.{EXT}")), "not ours").unwrap();
    let missing = d.join("does-not-exist");

    let found = discover(&[d.clone(), missing]);
    let mut names: Vec<String> = found
        .iter()
        .map(|(p, _)| p.file_name().unwrap().to_string_lossy().into_owned())
        .collect();
    names.sort();
    assert_eq!(
        names,
        [
            format!("winuxfs_backend_bad_abi.{EXT}"),
            format!("winuxfs_backend_bad_noentry.{EXT}"),
            format!("winuxfs_backend_bad_noops.{EXT}"),
        ]
    );
    assert!(found.iter().all(|(_, r)| r.is_err()));
}

#[test]
fn discover_loads_good_plugin() {
    let d = scratch_dir("discover-good");
    std::fs::copy(
        testplug(),
        d.join(format!("winuxfs_backend_testplug.{EXT}")),
    )
    .unwrap();
    let found = discover(&[d]);
    assert_eq!(found.len(), 1);
    let b = found[0].1.as_ref().unwrap();
    assert_eq!(b.name(), "testplug");
}

// ---- host API ----

struct RoDev(MemDevice);
impl BlockDevice for RoDev {
    fn size(&self) -> u64 {
        self.0.size()
    }
    fn sector_size(&self) -> u32 {
        self.0.sector_size()
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        self.0.read_at(off, buf)
    }
    fn write_at(&self, _off: u64, _buf: &[u8]) -> Result<(), Errno> {
        Err(Errno::EROFS)
    }
    fn flush(&self) -> Result<(), Errno> {
        Ok(())
    }
    fn read_only(&self) -> bool {
        true
    }
}

struct PanicDev;
impl BlockDevice for PanicDev {
    fn size(&self) -> u64 {
        panic!("size")
    }
    fn sector_size(&self) -> u32 {
        512
    }
    fn read_at(&self, _off: u64, _buf: &mut [u8]) -> Result<(), Errno> {
        panic!("boom")
    }
    fn write_at(&self, _off: u64, _buf: &[u8]) -> Result<(), Errno> {
        panic!("boom")
    }
    fn flush(&self) -> Result<(), Errno> {
        panic!("boom")
    }
    fn read_only(&self) -> bool {
        false
    }
}

#[test]
fn host_api_blk_roundtrip_via_function_pointers() {
    let dev: Arc<dyn BlockDevice> = Arc::new(MemDevice::new(4096, 512));
    let h = BlkHandle::new(dev.clone());
    let api = host_api();
    assert_eq!(api.abi_version, abi::WFS_ABI_VERSION);
    assert_eq!(api.size as usize, std::mem::size_of::<abi::wfs_host_api>());
    unsafe {
        assert_eq!((api.blk_size.unwrap())(h.as_ptr()), 4096);
        assert_eq!((api.blk_sector_size.unwrap())(h.as_ptr()), 512);
        assert_eq!((api.blk_read_only.unwrap())(h.as_ptr()), 0);
        let w = *b"abcd";
        assert_eq!(
            (api.blk_write.unwrap())(h.as_ptr(), 100, w.as_ptr().cast(), 4),
            0
        );
        let mut r = [0u8; 4];
        assert_eq!(
            (api.blk_read.unwrap())(h.as_ptr(), 100, r.as_mut_ptr().cast(), 4),
            0
        );
        assert_eq!(&r, b"abcd");
        assert_eq!(
            (api.blk_read.unwrap())(h.as_ptr(), 4096 - 2, r.as_mut_ptr().cast(), 4),
            -22
        );
        assert_eq!(
            (api.blk_read.unwrap())(h.as_ptr(), u64::MAX, r.as_mut_ptr().cast(), 4),
            -22
        );
        assert_eq!((api.blk_flush.unwrap())(h.as_ptr()), 0);
        // NULL device / NULL buffer with non-zero length are rejected, not dereferenced.
        let null = std::ptr::null_mut();
        assert_eq!(
            (api.blk_read.unwrap())(null, 0, r.as_mut_ptr().cast(), 4),
            -22
        );
        assert_eq!((api.blk_read.unwrap())(h.as_ptr(), 0, null.cast(), 4), -22);
        assert_eq!(
            (api.blk_write.unwrap())(h.as_ptr(), 0, std::ptr::null(), 4),
            -22
        );
        assert_eq!((api.blk_flush.unwrap())(null), -22);
        assert_eq!((api.blk_size.unwrap())(null), 0);
        (api.log.unwrap())(3, std::ptr::null());
        (api.log.unwrap())(0, c"host api log test".as_ptr());
    }
    let mut r = [0u8; 4];
    dev.read_at(100, &mut r).unwrap();
    assert_eq!(&r, b"abcd");
}

#[test]
fn host_api_write_on_read_only_is_erofs_even_len0() {
    let h = BlkHandle::new(Arc::new(RoDev(MemDevice::new(4096, 512))));
    let api = host_api();
    let b = [0u8; 4];
    unsafe {
        assert_eq!((api.blk_read_only.unwrap())(h.as_ptr()), 1);
        assert_eq!(
            (api.blk_write.unwrap())(h.as_ptr(), 0, b.as_ptr().cast(), 0),
            -30
        );
        assert_eq!(
            (api.blk_write.unwrap())(h.as_ptr(), 0, std::ptr::null(), 0),
            -30
        );
        assert_eq!(
            (api.blk_write.unwrap())(h.as_ptr(), 0, b.as_ptr().cast(), 4),
            -30
        );
    }
}

#[test]
fn panicking_blockdevice_does_not_unwind_into_c() {
    let h = BlkHandle::new(Arc::new(PanicDev));
    let api = host_api();
    let mut r = [0u8; 4];
    unsafe {
        assert_eq!(
            (api.blk_read.unwrap())(h.as_ptr(), 0, r.as_mut_ptr().cast(), 4),
            -5
        );
        assert_eq!(
            (api.blk_write.unwrap())(h.as_ptr(), 0, r.as_ptr().cast(), 4),
            -5
        );
        assert_eq!((api.blk_flush.unwrap())(h.as_ptr()), -5);
        assert_eq!((api.blk_size.unwrap())(h.as_ptr()), 0);
    }
}

// ---- Backend / Mount plumbing (tiny C test plugin) ----

#[test]
fn backend_metadata() {
    let b = load_plugin(&testplug()).unwrap();
    assert_eq!(b.name(), "testplug");
    assert_eq!(b.caps(), 0);
    let st = load_plugin(&test_plugin("testplug", "testplug_st")).unwrap();
    assert_eq!(st.caps(), abi::WFS_CAP_SINGLE_THREADED);
}

#[test]
fn probe_and_mount_use_host_block_api() {
    let b = load_plugin(&testplug()).unwrap();
    assert_eq!(b.probe(&no_devs()), Ok(false));
    let dev = Arc::new(MemDevice::new(4096, 512));
    dev.write_at(0, b"TPLGfrom-d").unwrap();
    let devs = || Devices {
        data: Some(dev.clone() as Arc<dyn BlockDevice>),
        log: None,
        rt: None,
    };
    assert_eq!(b.probe(&devs()), Ok(true));
    let m = b.mount(devs(), "").unwrap();
    let mut buf = [0u8; 16];
    let n = m.read(2, 0, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"from-d");
}

#[test]
fn mount_rejects_missing_required_and_disallowed_roles() {
    let b = load_plugin(&testplug()).unwrap();
    let d = || Some(Arc::new(MemDevice::new(4096, 512)) as Arc<dyn BlockDevice>);
    // LOG is not in dev_allowed.
    let r = b.mount(
        Devices {
            data: None,
            log: d(),
            rt: None,
        },
        "",
    );
    assert_eq!(r.err(), Some(Errno::EINVAL));
    let r = b.mount(
        Devices {
            data: None,
            log: None,
            rt: d(),
        },
        "",
    );
    assert_eq!(r.err(), Some(Errno::EINVAL));
    // DATA is required by the _st variant.
    let st = load_plugin(&test_plugin("testplug", "testplug_st")).unwrap();
    assert_eq!(st.mount(no_devs(), "").err(), Some(Errno::EINVAL));
    // Interior NUL in options cannot be passed to C.
    assert_eq!(b.mount(no_devs(), "a\0b").err(), Some(Errno::EINVAL));
}

#[test]
fn mount_ops_plumbing() {
    let b = load_plugin(&testplug()).unwrap();
    let m = b.mount(no_devs(), "").unwrap();

    let root = m.getattr(ROOT_INO).unwrap();
    assert_eq!(root.ino, 1);
    assert_eq!(root.ftype, FileType::Dir);
    assert_eq!(root.mode, 0o755);
    assert_eq!(root.nlink, 2);

    let st = m.lookup(ROOT_INO, b"hello").unwrap();
    assert_eq!(st.ino, 2);
    assert_eq!(st.ftype, FileType::Reg);
    assert_eq!(st.size, 6);
    m.forget(2, 1);
    assert_eq!(m.lookup(ROOT_INO, b"nope").err(), Some(Errno::ENOENT));
    assert_eq!(m.lookup(2, b"x").err(), Some(Errno::ENOTDIR));
    assert_eq!(m.getattr(99).err(), Some(Errno::ENOENT));

    let ents = m.readdir(ROOT_INO, 0, 16).unwrap();
    assert_eq!(ents.len(), 1);
    assert_eq!(ents[0].cookie, 1);
    assert_eq!(ents[0].ino, 2);
    assert_eq!(ents[0].ftype, FileType::Reg);
    assert_eq!(ents[0].name, b"hello");
    assert!(m.readdir(ROOT_INO, 1, 16).unwrap().is_empty());
    assert!(m.readdir(ROOT_INO, 0, 0).unwrap().is_empty());

    let mut buf = [0u8; 4];
    assert_eq!(m.read(2, 0, &mut buf), Ok(4));
    assert_eq!(&buf, b"hell");
    assert_eq!(m.read(2, 4, &mut buf), Ok(2));
    assert_eq!(&buf[..2], b"o\n");
    assert_eq!(m.read(2, 100, &mut buf), Ok(0));
    assert_eq!(m.read(1, 0, &mut buf).err(), Some(Errno::EISDIR));

    let sf = m.statfs().unwrap();
    assert_eq!(sf.total_bytes, 4096);
    assert_eq!(sf.block_size, 512);
    assert_eq!(sf.name_max, 255);
}

#[test]
fn null_optional_ops_are_enosys() {
    let b = load_plugin(&testplug()).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    let st = m.getattr(2).unwrap();
    assert_eq!(m.write(2, 0, b"x").err(), Some(Errno::ENOSYS));
    assert_eq!(
        m.create(ROOT_INO, b"f", FileType::Reg, 0o644, 0, 0).err(),
        Some(Errno::ENOSYS)
    );
    assert_eq!(m.unlink(ROOT_INO, b"hello"), Err(Errno::ENOSYS));
    assert_eq!(m.rmdir(ROOT_INO, b"d"), Err(Errno::ENOSYS));
    assert_eq!(
        m.rename(ROOT_INO, b"a", ROOT_INO, b"b", 0),
        Err(Errno::ENOSYS)
    );
    assert_eq!(m.setattr(2, &st, abi::WFS_SET_MODE), Err(Errno::ENOSYS));
    assert_eq!(m.sync(), Err(Errno::ENOSYS));
}

#[test]
fn backend_errno_is_sanitised() {
    let b = load_plugin(&testplug()).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    assert_eq!(m.getattr(1000).err(), Some(Errno::EIO)); // returned +7
    assert_eq!(m.getattr(1001).err(), Some(Errno::EIO)); // returned -1234
    let mut buf = [0u8; 4];
    assert_eq!(m.read(4, 0, &mut buf).err(), Some(Errno::EIO)); // done > len
    assert_eq!(MAX_ERRNO, 133);
}

#[test]
fn linux_errnos_pass_through_mount_wrapper() {
    let b = load_plugin(&testplug()).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    // Not among the Task 4 Errno constants, but valid Linux errnos.
    assert_eq!(m.getattr(2000 + 13).err(), Some(Errno(13))); // EACCES
    assert_eq!(m.getattr(2000 + 18).err(), Some(Errno(18))); // EXDEV
    assert_eq!(m.getattr(2000 + 1).err(), Some(Errno(1))); // EPERM
    assert_eq!(m.getattr(2000 + 117).err(), Some(Errno(117))); // EUCLEAN
                                                               // Range boundaries: EHWPOISON (133) passes, 134 does not.
    assert_eq!(m.getattr(2000 + 133).err(), Some(Errno(133)));
    assert_eq!(m.getattr(2000 + 134).err(), Some(Errno::EIO));
}

#[test]
fn readdir_rejects_cookies_at_or_above_2_pow_63() {
    let p = private_copy(&testplug(), "readdir-cookie");
    let b = load_plugin(&p).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    for dir in [9, 10, 11] {
        assert_eq!(m.readdir(dir, 0, 10).err(), Some(Errno::EIO), "dir {dir}");
    }
    // The root directory too.
    assert_eq!(m.readdir(1, 99, 10).err(), Some(Errno::EIO));
    // The largest valid cookie is accepted.
    let ents = m.readdir(12, 0, 10).unwrap();
    assert_eq!(ents.len(), 1);
    assert_eq!(ents[0].cookie, (1u64 << 63) - 1);
}

#[test]
fn readdir_bounded_when_backend_ignores_stop() {
    let p = private_copy(&testplug(), "readdir-stop");
    let b = load_plugin(&p).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    // dir 5 emits 1000 entries and ignores the callback's return value.
    let ents = m.readdir(5, 0, 10).unwrap();
    assert_eq!(ents.len(), 10);
    assert_eq!(ents[9].cookie, 10);
    assert_eq!(m.readdir(5, 0, 1).unwrap().len(), 1);
    // dir 6: good, bad, good, good. The bad entry fails the call and every
    // later callback is told to stop (no entry accepted after it).
    assert_eq!(m.readdir(6, 0, 100).err(), Some(Errno::EIO));
    let h = open_hooks(&p);
    assert_eq!(hook(&h, b"testplug_accepted_after_bad"), 0);
}

#[test]
fn bogus_child_from_lookup_and_create_is_eio_and_forgotten() {
    let p = private_copy(&testplug(), "bad-child");
    let b = load_plugin(&p).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    let h = open_hooks(&p);
    // Success with ino 0: EIO, nothing to forget.
    assert_eq!(m.lookup(ROOT_INO, b"zero").err(), Some(Errno::EIO));
    assert_eq!(hook(&h, b"testplug_bad_forgets"), 0);
    // Success with a bad ftype: EIO, and the reference on ino 7 is released.
    assert_eq!(m.lookup(ROOT_INO, b"badft").err(), Some(Errno::EIO));
    assert_eq!(hook(&h, b"testplug_bad_forgets"), 1);

    // Same through create (implemented only in the _st variant).
    let p = private_copy(&test_plugin("testplug", "testplug_st"), "bad-child-st");
    let b = load_plugin(&p).unwrap();
    let devs = Devices {
        data: Some(Arc::new(MemDevice::new(4096, 512))),
        log: None,
        rt: None,
    };
    let m = b.mount(devs, "").unwrap();
    let h = open_hooks(&p);
    let mk = |n: &[u8]| m.create(ROOT_INO, n, FileType::Reg, 0o644, 0, 0).err();
    assert_eq!(mk(b"zero"), Some(Errno::EIO));
    assert_eq!(hook(&h, b"testplug_bad_forgets"), 0);
    assert_eq!(mk(b"badft"), Some(Errno::EIO));
    assert_eq!(hook(&h, b"testplug_bad_forgets"), 1);
    assert_eq!(mk(b"other"), Some(Errno::EEXIST));
}

#[test]
fn mount_drop_unmounts_exactly_once_and_outlives_backend() {
    let p = private_copy(&testplug(), "unmount-once");
    let b = load_plugin(&p).unwrap();
    let m1 = b.mount(no_devs(), "").unwrap();
    let m2 = b.mount(no_devs(), "").unwrap();
    // The Backend value goes away; the Mounts must keep the library loaded.
    drop(b);
    // Opened only now: if the library had been unloaded with the Backend,
    // this would load a fresh instance and the counts below would not move.
    let h = open_hooks(&p);
    assert_eq!(hook(&h, b"testplug_unmount_count"), 0);
    drop(m1);
    assert_eq!(hook(&h, b"testplug_unmount_count"), 1);
    assert_eq!(m2.getattr(ROOT_INO).unwrap().ino, 1);
    let t = std::thread::spawn(move || {
        assert_eq!(m2.lookup(ROOT_INO, b"hello").unwrap().ino, 2);
        drop(m2);
    });
    t.join().unwrap();
    assert_eq!(hook(&h, b"testplug_unmount_count"), 2);
}

#[test]
fn mount_is_send_and_sync() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<Mount>();
    assert_send_sync::<Backend>();
}

fn hammer_slow_getattr(m: &Mount) {
    std::thread::scope(|s| {
        for _ in 0..8 {
            s.spawn(|| {
                for _ in 0..10 {
                    let _ = m.getattr(3);
                }
            });
        }
    });
}

#[test]
fn single_threaded_backend_calls_are_serialized() {
    let p = test_plugin("testplug", "testplug_st");
    let b = load_plugin(&p).unwrap();
    let devs = Devices {
        data: Some(Arc::new(MemDevice::new(4096, 512))),
        log: None,
        rt: None,
    };
    let m = b.mount(devs, "").unwrap();
    hammer_slow_getattr(&m);
    assert_eq!(hook(&open_hooks(&p), b"testplug_overlaps"), 0);
}

#[test]
fn multithreaded_backend_calls_overlap_control() {
    // Control for the test above: without WFS_CAP_SINGLE_THREADED the core
    // does not serialize, so the plugin's overlap detector must fire.
    let p = private_copy(&testplug(), "overlap-control");
    let b = load_plugin(&p).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    hammer_slow_getattr(&m);
    assert!(hook(&open_hooks(&p), b"testplug_overlaps") > 0);
}

// ---- ABI contract enforcement ----

#[test]
fn mount_rejects_ok_with_null_fs() {
    let p = private_copy(&testplug(), "null-fs");
    let b = load_plugin(&p).unwrap();
    assert_eq!(b.mount(no_devs(), "nullfs").err(), Some(Errno::EIO));
}

#[test]
fn mount_rejects_root_that_is_not_a_directory() {
    let p = private_copy(&testplug(), "bad-root");
    let b = load_plugin(&p).unwrap();
    let h = open_hooks(&p);
    assert_eq!(b.mount(no_devs(), "root=file").err(), Some(Errno::EIO));
    assert_eq!(b.mount(no_devs(), "root=err").err(), Some(Errno::EIO));
    // A mount that is refused is still unmounted, exactly once each.
    assert_eq!(hook(&h, b"testplug_unmount_count"), 2);
    assert!(b.mount(no_devs(), "").is_ok());
}

#[test]
fn readdir_entry_rules_are_enforced() {
    let p = private_copy(&testplug(), "readdir-rules");
    let b = load_plugin(&p).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    // 13 cookie 0; 14 repeated cookie; 15 cookie == input; 16 ino 0;
    // 17 empty name; 18 NUL in name; 19 '/' in name; 20 name of 256 bytes.
    for dir in 13..=20 {
        let cookie = if dir == 15 { 7 } else { 0 };
        assert_eq!(
            m.readdir(dir, cookie, 10).err(),
            Some(Errno::EIO),
            "dir {dir}"
        );
    }
    // 255 bytes is the longest valid name.
    let ents = m.readdir(21, 0, 10).unwrap();
    assert_eq!(ents[0].name.len(), 255);
}

#[test]
fn name_arguments_are_validated_before_the_backend() {
    let b = load_plugin(&testplug()).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    assert_eq!(m.lookup(1, &[b'a'; 256]).err(), Some(Errno::ENAMETOOLONG));
    for bad in [&b""[..], b"a\0b", b"a/b"] {
        assert_eq!(m.lookup(1, bad).err(), Some(Errno::EINVAL), "{bad:?}");
    }
    assert_eq!(m.lookup(1, b"nope").err(), Some(Errno::ENOENT));
}

#[test]
fn errno_values_are_linux_numbers_from_the_shipped_header() {
    // include/winuxfs/errno.h is the ABI's error space; the core's named
    // constants must agree with it.
    let h = std::fs::read_to_string(workspace_root().join("include/winuxfs/errno.h")).unwrap();
    let val = |name: &str| -> i32 {
        let key = format!("#define WFS_{name} ");
        let l = h
            .lines()
            .find(|l| l.starts_with(&key))
            .unwrap_or_else(|| panic!("{name} missing from errno.h"));
        l[key.len()..].trim().parse().unwrap()
    };
    for (n, e) in [
        ("ENOENT", Errno::ENOENT),
        ("EIO", Errno::EIO),
        ("EBADF", Errno::EBADF),
        ("EACCES", Errno::EACCES),
        ("EBUSY", Errno::EBUSY),
        ("EEXIST", Errno::EEXIST),
        ("ENOTDIR", Errno::ENOTDIR),
        ("EISDIR", Errno::EISDIR),
        ("EINVAL", Errno::EINVAL),
        ("EFBIG", Errno::EFBIG),
        ("ENOSPC", Errno::ENOSPC),
        ("EROFS", Errno::EROFS),
        ("ENAMETOOLONG", Errno::ENAMETOOLONG),
        ("ENOSYS", Errno::ENOSYS),
        ("ENOTEMPTY", Errno::ENOTEMPTY),
        ("EOVERFLOW", Errno::EOVERFLOW),
    ] {
        assert_eq!(val(n), e.0, "{n}");
    }
    assert_eq!(val("EHWPOISON"), MAX_ERRNO);
    // A backend that returns the C runtime's number instead (mingw ENOTEMPTY
    // is 41) is NOT translated: the host reads 41 as the Linux errno
    // EMULTIHOP. Backends must use winuxfs/errno.h.
    let b = load_plugin(&testplug()).unwrap();
    let m = b.mount(no_devs(), "").unwrap();
    assert_eq!(m.getattr(2000 + 41).err(), Some(Errno(41)));
    assert_ne!(Errno(41), Errno::ENOTEMPTY);
}

#[test]
fn explicit_unmount_is_idempotent_and_reports_once() {
    let p = private_copy(&testplug(), "explicit-unmount");
    let b = load_plugin(&p).unwrap();
    let h = open_hooks(&p);
    let m = b.mount(no_devs(), "").unwrap();
    assert_eq!(m.unmount(), Ok(()));
    assert_eq!(m.unmount(), Ok(()));
    drop(m);
    assert_eq!(hook(&h, b"testplug_unmount_count"), 1);
}

struct FlushCounter {
    flushes: std::sync::atomic::AtomicUsize,
    ro: bool,
}

impl BlockDevice for FlushCounter {
    fn size(&self) -> u64 {
        4096
    }
    fn sector_size(&self) -> u32 {
        512
    }
    fn read_at(&self, _off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        buf.fill(0);
        Ok(())
    }
    fn write_at(&self, _off: u64, _buf: &[u8]) -> Result<(), Errno> {
        Ok(())
    }
    fn flush(&self) -> Result<(), Errno> {
        self.flushes
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        Ok(())
    }
    fn read_only(&self) -> bool {
        self.ro
    }
}

#[test]
fn unmount_flushes_writable_devices_only() {
    use std::sync::atomic::Ordering::SeqCst;
    let b = load_plugin(&test_plugin("testplug", "testplug_st")).unwrap();
    for (ro, expect) in [(false, 1), (true, 0)] {
        let d = Arc::new(FlushCounter {
            flushes: Default::default(),
            ro,
        });
        let devs = Devices {
            data: Some(d.clone()),
            log: None,
            rt: None,
        };
        let m = b.mount(devs, "").unwrap();
        assert_eq!(d.flushes.load(SeqCst), 0);
        assert_eq!(m.unmount(), Ok(()));
        assert_eq!(d.flushes.load(SeqCst), expect, "ro={ro}");
        drop(m);
        assert_eq!(d.flushes.load(SeqCst), expect, "flush happens once");
    }
}

#[test]
fn no_backend_call_after_unmount() {
    let p = private_copy(&testplug(), "no-call-after-unmount");
    let b = load_plugin(&p).unwrap();
    let h = open_hooks(&p);
    let m = b.mount(no_devs(), "").unwrap();
    assert!(m.getattr(1).is_ok());
    assert_eq!(m.unmount(), Ok(()));
    let before = hook(&h, b"testplug_calls");
    assert!(before > 0);
    assert_eq!(m.getattr(1).err(), Some(Errno::EIO));
    assert_eq!(m.lookup(1, b"hello").err(), Some(Errno::EIO));
    assert_eq!(m.readdir(1, 0, 10).err(), Some(Errno::EIO));
    assert_eq!(m.read(2, 0, &mut [0u8; 6]).err(), Some(Errno::EIO));
    assert_eq!(m.statfs().err(), Some(Errno::EIO));
    m.forget(2, 1);
    assert_eq!(m.unmount(), Ok(()));
    assert_eq!(hook(&h, b"testplug_calls"), before, "backend reached");
    drop(m);
    assert_eq!(hook(&h, b"testplug_calls"), before);
    assert_eq!(hook(&h, b"testplug_unmount_count"), 1, "double unmount");
}

#[test]
fn vfs_unmount_reports_backend_error_and_blocks_later_calls() {
    let p = private_copy(&testplug(), "vfs-unmount-err");
    let b = load_plugin(&p).unwrap();
    let h = open_hooks(&p);
    let m = Arc::new(b.mount(no_devs(), "unmount=err").unwrap());
    let vfs = Vfs::new(
        m.clone(),
        VfsOptions {
            case_insensitive: false,
            read_only: true,
        },
    );
    assert_eq!(vfs.unmount(), Err(Errno::EIO));
    assert_eq!(vfs.unmount(), Ok(()));
    let before = hook(&h, b"testplug_calls");
    assert_eq!(m.getattr(1).err(), Some(Errno::EIO));
    assert_eq!(hook(&h, b"testplug_calls"), before);
    drop(vfs);
    drop(m);
    assert_eq!(hook(&h, b"testplug_unmount_count"), 1);
}
