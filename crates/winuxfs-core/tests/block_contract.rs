use std::sync::Arc;
use winuxfs_core::*;

fn contract(dev: &dyn BlockDevice) {
    let size = dev.size();
    dev.write_at(size - 5, b"hello").unwrap();
    let mut b = [0u8; 5];
    dev.read_at(size - 5, &mut b).unwrap();
    assert_eq!(&b, b"hello");
    assert_eq!(dev.read_at(size - 4, &mut [0; 5]), Err(Errno::EINVAL));
    assert_eq!(dev.write_at(size - 4, &[0; 5]), Err(Errno::EINVAL));
    assert_eq!(dev.read_at(u64::MAX, &mut [0; 2]), Err(Errno::EINVAL));
    assert_eq!(dev.write_at(u64::MAX, &[0; 2]), Err(Errno::EINVAL));
    assert_eq!(dev.read_at(size + 100, &mut []), Ok(()));
    assert_eq!(dev.write_at(size + 100, &[]), Ok(()));
    assert_eq!(dev.flush(), Ok(()));
}

fn tmp(name: &str, len: u64) -> std::path::PathBuf {
    let p = std::env::temp_dir().join(format!("winuxfs-{}-{}", name, std::process::id()));
    std::fs::File::create(&p).unwrap().set_len(len).unwrap();
    p
}

#[test]
fn file_device() {
    let p = tmp("file", 8192);
    let d = FileDevice::open(&p, false).unwrap();
    assert_eq!(d.size(), 8192);
    assert_eq!(d.sector_size(), 512);
    assert!(!d.read_only());
    contract(&d);
    drop(d);
    let ro = FileDevice::open(&p, true).unwrap();
    assert!(ro.read_only());
    assert_eq!(ro.write_at(0, b"x"), Err(Errno::EROFS));
    let mut b = [0u8; 5];
    ro.read_at(8192 - 5, &mut b).unwrap();
    assert_eq!(&b, b"hello");
    assert!(FileDevice::open(&p.with_extension("nope"), true).is_err());
    std::fs::remove_file(p).unwrap();
}

#[test]
fn mem_device() {
    contract(&MemDevice::new(8192, 512));
}

#[test]
fn aligned_over_strict_4096() {
    let inner = Arc::new(MemDevice::strict(16384, 4096));
    assert_eq!(inner.write_at(1, b"abc"), Err(Errno::EINVAL));
    inner.write_at(0, &[0xAA; 16384]).unwrap();
    let d = AlignedDevice::new(inner.clone());
    assert_eq!(d.sector_size(), 4096);
    contract(&d);
    d.write_at(4096 + 1, b"xyz").unwrap();
    let mut b = [0u8; 8];
    d.read_at(4096, &mut b).unwrap();
    assert_eq!(b, [0xAA, b'x', b'y', b'z', 0xAA, 0xAA, 0xAA, 0xAA]);
    // spanning two sectors
    d.write_at(8190, b"abcd").unwrap();
    let mut c = [0u8; 6];
    d.read_at(8189, &mut c).unwrap();
    assert_eq!(&c, &[0xAA, b'a', b'b', b'c', b'd', 0xAA]);
}

#[test]
fn aligned_concurrent_adjacent_writes() {
    let inner = Arc::new(MemDevice::strict(4096, 4096));
    let d = Arc::new(AlignedDevice::new(inner));
    for _ in 0..200 {
        let (a, b) = (d.clone(), d.clone());
        let t1 = std::thread::spawn(move || a.write_at(10, &[1; 10]).unwrap());
        let t2 = std::thread::spawn(move || b.write_at(20, &[2; 10]).unwrap());
        t1.join().unwrap();
        t2.join().unwrap();
        let mut buf = [0u8; 20];
        d.read_at(10, &mut buf).unwrap();
        assert_eq!(&buf[..10], &[1; 10]);
        assert_eq!(&buf[10..], &[2; 10]);
        d.write_at(10, &[0; 20]).unwrap();
    }
}
