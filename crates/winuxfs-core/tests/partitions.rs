use proptest::prelude::*;
use std::sync::Arc;
use winuxfs_core::part::{scan, PartError, PartKind, PartitionView, PartitionInfo};
use winuxfs_core::{BlockDevice, Errno, MemDevice};

const SS: usize = 512;

fn crc32(d: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in d {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

fn put(dev: &MemDevice, off: usize, b: &[u8]) {
    dev.write_at(off as u64, b).unwrap();
}

fn mbr_entry(dev: &MemDevice, sector_off: usize, slot: usize, ty: u8, start: u32, len: u32) {
    let mut e = [0u8; 16];
    e[4] = ty;
    e[8..12].copy_from_slice(&start.to_le_bytes());
    e[12..16].copy_from_slice(&len.to_le_bytes());
    put(dev, sector_off + 446 + slot * 16, &e);
    put(dev, sector_off + 510, &[0x55, 0xAA]);
}

fn gpt_disk(sectors: usize, parts: &[(u64, u64, &str)]) -> MemDevice {
    let dev = MemDevice::new((sectors * SS) as u64, 512);
    mbr_entry(&dev, 0, 0, 0xEE, 1, (sectors - 1) as u32);
    let mut arr = vec![0u8; 128 * 128];
    for (i, (s, e, name)) in parts.iter().enumerate() {
        let b = &mut arr[i * 128..(i + 1) * 128];
        b[0..16].copy_from_slice(&[0xAF; 16]);
        b[16..32].copy_from_slice(&[i as u8 + 1; 16]);
        b[32..40].copy_from_slice(&s.to_le_bytes());
        b[40..48].copy_from_slice(&e.to_le_bytes());
        for (j, u) in name.encode_utf16().enumerate() {
            b[56 + j * 2..58 + j * 2].copy_from_slice(&u.to_le_bytes());
        }
    }
    let last = (sectors - 1) as u64;
    for (hdr_lba, other, ent_lba) in [(1u64, last, 2u64), (last, 1, last - 32)] {
        let mut h = vec![0u8; 92];
        h[0..8].copy_from_slice(b"EFI PART");
        h[8..12].copy_from_slice(&0x0001_0000u32.to_le_bytes());
        h[12..16].copy_from_slice(&92u32.to_le_bytes());
        h[24..32].copy_from_slice(&hdr_lba.to_le_bytes());
        h[32..40].copy_from_slice(&other.to_le_bytes());
        h[40..48].copy_from_slice(&34u64.to_le_bytes());
        h[48..56].copy_from_slice(&(last - 33).to_le_bytes());
        h[72..80].copy_from_slice(&ent_lba.to_le_bytes());
        h[80..84].copy_from_slice(&128u32.to_le_bytes());
        h[84..88].copy_from_slice(&128u32.to_le_bytes());
        h[88..92].copy_from_slice(&crc32(&arr).to_le_bytes());
        let c = crc32(&h);
        h[16..20].copy_from_slice(&c.to_le_bytes());
        put(&dev, hdr_lba as usize * SS, &h);
        put(&dev, ent_lba as usize * SS, &arr);
    }
    dev
}

#[test]
fn no_table_returns_empty() {
    let dev = MemDevice::new(64 * 512, 512);
    assert_eq!(scan(&dev).unwrap(), vec![]);
}

#[test]
fn mbr_primary_two_partitions() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, 2048, 1000);
    mbr_entry(&dev, 0, 1, 0x07, 4000, 96);
    let p = scan(&dev).unwrap();
    assert_eq!(p.len(), 2);
    assert_eq!((p[0].index, p[0].start, p[0].len), (1, 2048 * 512, 1000 * 512));
    assert!(matches!(p[0].kind, PartKind::Mbr { type_id: 0x83 }));
    assert_eq!((p[1].index, p[1].start, p[1].len), (2, 4000 * 512, 96 * 512));
}

#[test]
fn mbr_extended_chain() {
    let dev = MemDevice::new(8192 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x05, 100, 4000);
    // EBR at 100: logical at +1 (rel to EBR), next EBR at rel 1000 (rel to ext start)
    for (i, ebr) in [100u32, 1100, 2100].iter().enumerate() {
        mbr_entry(&dev, *ebr as usize * 512, 0, 0x83, 1, 500);
        if i < 2 {
            mbr_entry(&dev, *ebr as usize * 512, 1, 0x05, 1000 * (i as u32 + 1), 1000);
        }
    }
    let p = scan(&dev).unwrap();
    let idx: Vec<u32> = p.iter().map(|x| x.index).collect();
    assert_eq!(idx, vec![5, 6, 7]);
    assert_eq!(p[0].start, 101 * 512);
    assert_eq!(p[1].start, 1101 * 512);
    assert_eq!(p[2].start, 2101 * 512);
    assert_eq!(p[2].len, 500 * 512);
}

#[test]
fn extended_cycle_terminates() {
    let dev = MemDevice::new(8192 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x05, 100, 4000);
    mbr_entry(&dev, 100 * 512, 0, 0x83, 1, 10);
    mbr_entry(&dev, 100 * 512, 1, 0x05, 0, 1000);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn gpt_basic_and_backup_fallback() {
    let dev = gpt_disk(2048, &[(34, 99, "boot"), (100, 999, "data")]);
    let p = scan(&dev).unwrap();
    assert_eq!(p.len(), 2);
    assert_eq!((p[0].index, p[0].start, p[0].len), (1, 34 * 512, 66 * 512));
    assert_eq!((p[1].index, p[1].start, p[1].len), (2, 100 * 512, 900 * 512));
    match &p[1].kind {
        PartKind::Gpt { type_guid, name } => {
            assert_eq!(*type_guid, [0xAF; 16]);
            assert_eq!(name, "data");
        }
        _ => panic!(),
    }
    put(&dev, 512 + 16, &[0xFF, 0xFF, 0xFF, 0xFF]); // break primary header CRC
    assert_eq!(scan(&dev).unwrap(), p);
}

#[test]
fn gpt_protective_mbr_is_not_listed() {
    let dev = gpt_disk(2048, &[(34, 99, "a")]);
    let p = scan(&dev).unwrap();
    assert_eq!(p.len(), 1);
    assert!(matches!(p[0].kind, PartKind::Gpt { .. }));
}

#[test]
fn gpt_both_headers_bad_is_corrupt() {
    let dev = gpt_disk(2048, &[(34, 99, "a")]);
    put(&dev, 512 + 16, &[1, 2, 3, 4]);
    put(&dev, 2047 * 512 + 16, &[1, 2, 3, 4]);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn partition_beyond_device_is_corrupt() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, 2048, 5000);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, u32::MAX, u32::MAX);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn view_translates_offsets_and_rejects_overflow() {
    let dev = Arc::new(MemDevice::new(4096 * 512, 512));
    put(&dev, 2048 * 512, b"hello");
    let info = PartitionInfo {
        index: 1,
        start: 2048 * 512,
        len: 1000 * 512,
        kind: PartKind::Mbr { type_id: 0x83 },
    };
    let v = PartitionView::new(dev.clone(), &info).unwrap();
    assert_eq!(v.size(), 1000 * 512);
    let mut b = [0u8; 5];
    v.read_at(0, &mut b).unwrap();
    assert_eq!(&b, b"hello");
    v.write_at(512, b"world").unwrap();
    let mut c = [0u8; 5];
    dev.read_at(2049 * 512, &mut c).unwrap();
    assert_eq!(&c, b"world");
    assert_eq!(v.read_at(1000 * 512 - 2, &mut b), Err(Errno::EINVAL));
    assert_eq!(v.write_at(u64::MAX, &b), Err(Errno::EINVAL));
    let big = PartitionInfo { len: 1 << 40, ..info };
    assert_eq!(PartitionView::new(dev, &big).err(), Some(Errno::EINVAL));
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]
    #[test]
    fn random_bytes_never_panic(bytes in proptest::collection::vec(any::<u8>(), 64 * 512)) {
        let dev = MemDevice::new(64 * 512, 512);
        put(&dev, 0, &bytes);
        let _ = scan(&dev);
    }

    // Structured: valid-looking signatures with random fields.
    #[test]
    fn random_fields_with_signatures_never_panic(bytes in proptest::collection::vec(any::<u8>(), 64 * 512), ty in any::<u8>()) {
        let dev = MemDevice::new(64 * 512, 512);
        put(&dev, 0, &bytes);
        put(&dev, 510, &[0x55, 0xAA]);
        put(&dev, 446 + 4, &[ty]);
        put(&dev, 512, b"EFI PART");
        let _ = scan(&dev);
    }
}
