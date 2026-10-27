use proptest::prelude::*;
use std::sync::Arc;
use winuxfs_core::part::{scan, PartError, PartKind, PartitionInfo, PartitionView};
use winuxfs_core::{BlockDevice, Errno, MemDevice};

const SS: usize = 512;

fn crc32(d: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in d {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 {
                (c >> 1) ^ 0xEDB8_8320
            } else {
                c >> 1
            };
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

fn gpt_arr(parts: &[(u64, u64, &str)]) -> Vec<u8> {
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
    arr
}

/// Full control: header byte mutations are applied to the PRIMARY header only;
/// `fix_crc` recomputes its header CRC afterwards. `arr_crc_xor` corrupts the
/// stored entry-array CRC in both headers.
fn gpt_build(
    sectors: usize,
    arr: &[u8],
    usable: (u64, u64),
    rev: u32,
    primary_muts: &[(usize, u8)],
    fix_crc: bool,
    arr_crc_xor: u32,
) -> MemDevice {
    let dev = MemDevice::new((sectors * SS) as u64, 512);
    mbr_entry(&dev, 0, 0, 0xEE, 1, (sectors - 1) as u32);
    let last = (sectors - 1) as u64;
    for (hdr_lba, other, ent_lba) in [(1u64, last, 2u64), (last, 1, last - 32)] {
        let mut h = vec![0u8; 92];
        h[0..8].copy_from_slice(b"EFI PART");
        h[8..12].copy_from_slice(&rev.to_le_bytes());
        h[12..16].copy_from_slice(&92u32.to_le_bytes());
        h[24..32].copy_from_slice(&hdr_lba.to_le_bytes());
        h[32..40].copy_from_slice(&other.to_le_bytes());
        h[40..48].copy_from_slice(&usable.0.to_le_bytes());
        h[48..56].copy_from_slice(&usable.1.to_le_bytes());
        h[72..80].copy_from_slice(&ent_lba.to_le_bytes());
        h[80..84].copy_from_slice(&128u32.to_le_bytes());
        h[84..88].copy_from_slice(&128u32.to_le_bytes());
        h[88..92].copy_from_slice(&(crc32(arr) ^ arr_crc_xor).to_le_bytes());
        let c = crc32(&h);
        h[16..20].copy_from_slice(&c.to_le_bytes());
        if hdr_lba == 1 {
            for &(o, v) in primary_muts {
                h[o % 92] = v;
            }
            if fix_crc {
                h[16..20].fill(0);
                let c = crc32(&h);
                h[16..20].copy_from_slice(&c.to_le_bytes());
            }
        }
        put(&dev, hdr_lba as usize * SS, &h);
        put(&dev, ent_lba as usize * SS, arr);
    }
    dev
}

fn gpt_disk(sectors: usize, parts: &[(u64, u64, &str)]) -> MemDevice {
    gpt_build(
        sectors,
        &gpt_arr(parts),
        (34, sectors as u64 - 34),
        0x0001_0000,
        &[],
        false,
        0,
    )
}

fn mbr_ext_disk() -> MemDevice {
    let dev = MemDevice::new(8192 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, 10, 50);
    mbr_entry(&dev, 0, 1, 0x05, 100, 4000);
    for (i, ebr) in [100u32, 1100, 2100].iter().enumerate() {
        mbr_entry(&dev, *ebr as usize * 512, 0, 0x83, 1, 500);
        if i < 2 {
            mbr_entry(
                &dev,
                *ebr as usize * 512,
                1,
                0x05,
                1000 * (i as u32 + 1),
                1000,
            );
        }
    }
    dev
}

fn check_invariants(p: &[PartitionInfo], size: u64) {
    let mut prev_idx = 0;
    for x in p {
        assert!(x.index > prev_idx, "indexes sorted and unique");
        prev_idx = x.index;
        assert!(x.start.checked_add(x.len).unwrap() <= size, "within device");
    }
    let mut r: Vec<(u64, u64)> = p.iter().map(|x| (x.start, x.len)).collect();
    r.sort();
    for w in r.windows(2) {
        assert!(w[0].0 + w[0].1 <= w[1].0, "non-overlapping");
    }
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
    assert_eq!(
        (p[0].index, p[0].start, p[0].len),
        (1, 2048 * 512, 1000 * 512)
    );
    assert!(matches!(p[0].kind, PartKind::Mbr { type_id: 0x83 }));
    assert_eq!(
        (p[1].index, p[1].start, p[1].len),
        (2, 4000 * 512, 96 * 512)
    );
}

#[test]
fn mbr_extended_chain() {
    let dev = MemDevice::new(8192 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x05, 100, 4000);
    // EBR at 100: logical at +1 (rel to EBR), next EBR at rel 1000 (rel to ext start)
    for (i, ebr) in [100u32, 1100, 2100].iter().enumerate() {
        mbr_entry(&dev, *ebr as usize * 512, 0, 0x83, 1, 500);
        if i < 2 {
            mbr_entry(
                &dev,
                *ebr as usize * 512,
                1,
                0x05,
                1000 * (i as u32 + 1),
                1000,
            );
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
    assert_eq!(
        (p[1].index, p[1].start, p[1].len),
        (2, 100 * 512, 900 * 512)
    );
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
    let big = PartitionInfo {
        len: 1 << 40,
        ..info
    };
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

    #[test]
    fn mutated_mbr_chain_never_panics(
        muts in proptest::collection::vec((0usize..4, 440usize..512, any::<u8>()), 1..8)
    ) {
        let dev = mbr_ext_disk();
        for (sec, o, v) in muts {
            let base = [0usize, 100, 1100, 2100][sec] * 512;
            put(&dev, base + o, &[v]);
        }
        if let Ok(p) = scan(&dev) {
            check_invariants(&p, dev.size());
        }
    }

    #[test]
    fn mutated_gpt_never_panics(
        arr_muts in proptest::collection::vec((0usize..512, any::<u8>()), 0..8),
        hdr_muts in proptest::collection::vec((8usize..92, any::<u8>()), 0..4),
        fix in any::<bool>(),
    ) {
        let mut arr = gpt_arr(&[(34, 99, "a"), (100, 999, "b"), (1000, 1500, "c")]);
        for (o, v) in arr_muts { arr[o] = v; }
        let dev = gpt_build(2048, &arr, (34, 2014), 0x0001_0000, &hdr_muts, fix, 0);
        if let Ok(p) = scan(&dev) {
            check_invariants(&p, dev.size());
        }
    }
}

#[test]
fn gpt_entry_array_crc_mismatch_is_corrupt() {
    let arr = gpt_arr(&[(34, 99, "a")]);
    let dev = gpt_build(2048, &arr, (34, 2014), 0x0001_0000, &[], false, 1);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn gpt_entry_at_lba1_rejected() {
    let dev = gpt_build(
        2048,
        &gpt_arr(&[(1, 99, "a")]),
        (34, 2014),
        0x0001_0000,
        &[],
        false,
        0,
    );
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn gpt_entry_past_last_usable_rejected() {
    let dev = gpt_build(
        2048,
        &gpt_arr(&[(100, 2020, "a")]),
        (34, 2014),
        0x0001_0000,
        &[],
        false,
        0,
    );
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn gpt_first_usable_after_last_usable_rejected() {
    let dev = gpt_build(2048, &gpt_arr(&[]), (500, 100), 0x0001_0000, &[], false, 0);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn gpt_bad_revision_rejected() {
    let dev = gpt_build(
        2048,
        &gpt_arr(&[(34, 99, "a")]),
        (34, 2014),
        0x0002_0000,
        &[],
        false,
        0,
    );
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn gpt_overlapping_entries_rejected() {
    let dev = gpt_disk(2048, &[(34, 99, "a"), (90, 200, "b")]);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn mbr_entry_at_lba0_rejected() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, 0, 100);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn mbr_primaries_overlap_rejected() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, 100, 100);
    mbr_entry(&dev, 0, 1, 0x83, 150, 100);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn mbr_primary_overlaps_extended_rejected() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x83, 100, 100);
    mbr_entry(&dev, 0, 1, 0x05, 150, 1000);
    mbr_entry(&dev, 150 * 512, 0, 0x83, 1, 10);
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn mbr_logical_outside_extended_rejected() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x05, 100, 200);
    mbr_entry(&dev, 100 * 512, 0, 0x83, 1, 500); // ends at 601 > 300
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn mbr_logicals_overlap_rejected() {
    let dev = MemDevice::new(4096 * 512, 512);
    mbr_entry(&dev, 0, 0, 0x05, 100, 2000);
    mbr_entry(&dev, 100 * 512, 0, 0x83, 1, 600);
    mbr_entry(&dev, 100 * 512, 1, 0x05, 300, 1000);
    mbr_entry(&dev, 400 * 512, 0, 0x83, 1, 100); // starts at 401, inside first logical
    assert!(matches!(scan(&dev), Err(PartError::Corrupt(_))));
}

#[test]
fn mbr_valid_mixed_disk_scans() {
    let p = scan(&mbr_ext_disk()).unwrap();
    assert_eq!(
        p.iter().map(|x| x.index).collect::<Vec<_>>(),
        vec![1, 5, 6, 7]
    );
    check_invariants(&p, 8192 * 512);
}

#[test]
fn works_on_4096_sector_device() {
    // LBA unit is max(sector_size, 512) = 4096
    let dev = MemDevice::new(1024 * 4096, 4096);
    let mut e = [0u8; 16];
    e[4] = 0x83;
    e[8..12].copy_from_slice(&10u32.to_le_bytes());
    e[12..16].copy_from_slice(&20u32.to_le_bytes());
    put(&dev, 446, &e);
    put(&dev, 510, &[0x55, 0xAA]);
    let p = scan(&dev).unwrap();
    assert_eq!((p[0].start, p[0].len), (10 * 4096, 20 * 4096));
}
