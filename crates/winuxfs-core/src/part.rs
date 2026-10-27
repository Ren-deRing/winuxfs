//! MBR / GPT partition table scanning and a partition-relative block view.

use crate::{BlockDevice, Errno};
use std::sync::Arc;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PartKind {
    Mbr { type_id: u8 },
    Gpt { type_guid: [u8; 16], name: String },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PartitionInfo {
    /// 1-based. MBR logical partitions start at 5.
    pub index: u32,
    /// Byte offset on the device.
    pub start: u64,
    /// Length in bytes.
    pub len: u64,
    pub kind: PartKind,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PartError {
    Io(Errno),
    Corrupt(&'static str),
}

impl From<Errno> for PartError {
    fn from(e: Errno) -> Self {
        PartError::Io(e)
    }
}

impl std::fmt::Display for PartError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PartError::Io(e) => write!(f, "partition scan I/O error: {e}"),
            PartError::Corrupt(m) => write!(f, "corrupt partition table: {m}"),
        }
    }
}

impl std::error::Error for PartError {}

const MAX_EXT_DEPTH: usize = 128;
const MAX_GPT_ENTRIES: u32 = 4096;
const MAX_GPT_ESIZE: u32 = 1024;

/// CRC-32 (IEEE, reflected, poly 0xEDB88320).
fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn le64(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

struct Ctx<'a> {
    dev: &'a dyn BlockDevice,
    ss: u64,
    size: u64,
}

impl Ctx<'_> {
    fn read_sector(&self, lba: u64) -> Result<Vec<u8>, PartError> {
        let off = lba.checked_mul(self.ss).ok_or(PartError::Corrupt("lba overflow"))?;
        let mut b = vec![0u8; self.ss as usize];
        self.dev.read_at(off, &mut b)?;
        Ok(b)
    }

    /// Converts a sector range to a checked (byte start, byte len).
    fn range(&self, start_lba: u64, sectors: u64) -> Result<(u64, u64), PartError> {
        let start = start_lba.checked_mul(self.ss).ok_or(PartError::Corrupt("lba overflow"))?;
        let len = sectors.checked_mul(self.ss).ok_or(PartError::Corrupt("length overflow"))?;
        let end = start.checked_add(len).ok_or(PartError::Corrupt("range overflow"))?;
        if end > self.size {
            return Err(PartError::Corrupt("partition extends beyond device"));
        }
        Ok((start, len))
    }
}

/// Rejects overlapping `(start, len)` byte ranges.
fn check_overlap(mut r: Vec<(u64, u64)>) -> Result<(), PartError> {
    r.sort();
    for w in r.windows(2) {
        // ranges were already bounds-checked, so start + len cannot overflow
        if w[0].0 + w[0].1 > w[1].0 {
            return Err(PartError::Corrupt("overlapping partitions"));
        }
    }
    Ok(())
}

struct MbrEntry {
    ty: u8,
    start: u32,
    len: u32,
}

fn mbr_entries(sec: &[u8]) -> Result<[MbrEntry; 4], PartError> {
    if sec[510] != 0x55 || sec[511] != 0xAA {
        return Err(PartError::Corrupt("bad MBR signature"));
    }
    let e = |i: usize| {
        let o = 446 + i * 16;
        MbrEntry { ty: sec[o + 4], start: le32(sec, o + 8), len: le32(sec, o + 12) }
    };
    Ok([e(0), e(1), e(2), e(3)])
}

fn is_extended(ty: u8) -> bool {
    matches!(ty, 0x05 | 0x0F | 0x85)
}

pub fn scan(dev: &dyn BlockDevice) -> Result<Vec<PartitionInfo>, PartError> {
    let ss = (dev.sector_size() as u64).max(512);
    let cx = Ctx { dev, ss, size: dev.size() };
    if cx.size < ss {
        return Ok(vec![]);
    }
    let sec0 = cx.read_sector(0)?;
    if sec0[510] != 0x55 || sec0[511] != 0xAA {
        return Ok(vec![]);
    }
    let ents = mbr_entries(&sec0)?;
    if ents.iter().any(|e| e.ty == 0xEE) {
        return scan_gpt(&cx);
    }
    let mut out = Vec::new();
    let mut top = Vec::new(); // primaries + extended containers
    for (i, e) in ents.iter().enumerate() {
        if e.ty == 0 || e.len == 0 {
            continue;
        }
        if e.start == 0 {
            return Err(PartError::Corrupt("partition overlaps MBR"));
        }
        let (start, len) = cx.range(e.start as u64, e.len as u64)?;
        top.push((start, len));
        if is_extended(e.ty) {
            scan_extended(&cx, e.start as u64, (start, len), &mut out)?;
        } else {
            out.push(PartitionInfo {
                index: i as u32 + 1,
                start,
                len,
                kind: PartKind::Mbr { type_id: e.ty },
            });
        }
    }
    check_overlap(top)?;
    check_overlap(out.iter().filter(|p| p.index >= 5).map(|p| (p.start, p.len)).collect())?;
    // Primaries (1..=4) first, then logicals (5..) in chain order.
    out.sort_by_key(|p| p.index);
    Ok(out)
}

fn scan_extended(
    cx: &Ctx,
    ext_start: u64,
    container: (u64, u64),
    out: &mut Vec<PartitionInfo>,
) -> Result<(), PartError> {
    let mut ebr = ext_start;
    for n in 0..MAX_EXT_DEPTH {
        let sec = cx.read_sector(ebr)?;
        let ents = mbr_entries(&sec)?;
        let l = &ents[0];
        if l.ty != 0 && l.len != 0 {
            let abs = ebr.checked_add(l.start as u64).ok_or(PartError::Corrupt("lba overflow"))?;
            let (start, len) = cx.range(abs, l.len as u64)?;
            if start < container.0 || start + len > container.0 + container.1 {
                return Err(PartError::Corrupt("logical partition outside extended"));
            }
            out.push(PartitionInfo {
                index: 5 + n as u32,
                start,
                len,
                kind: PartKind::Mbr { type_id: l.ty },
            });
        }
        let nx = &ents[1];
        if nx.ty == 0 || nx.len == 0 || !is_extended(nx.ty) {
            return Ok(());
        }
        ebr = ext_start
            .checked_add(nx.start as u64)
            .ok_or(PartError::Corrupt("lba overflow"))?;
        let (eb, _) = cx.range(ebr, 1)?;
        if eb < container.0 || eb + cx.ss > container.0 + container.1 {
            return Err(PartError::Corrupt("EBR outside extended"));
        }
    }
    Err(PartError::Corrupt("extended partition chain too deep"))
}

fn scan_gpt(cx: &Ctx) -> Result<Vec<PartitionInfo>, PartError> {
    let last = cx.size / cx.ss - 1;
    match try_gpt(cx, 1) {
        Ok(v) => Ok(v),
        Err(PartError::Io(e)) => Err(PartError::Io(e)),
        Err(_) => try_gpt(cx, last),
    }
}

fn try_gpt(cx: &Ctx, hdr_lba: u64) -> Result<Vec<PartitionInfo>, PartError> {
    let sec = cx.read_sector(hdr_lba)?;
    if &sec[0..8] != b"EFI PART" {
        return Err(PartError::Corrupt("bad GPT signature"));
    }
    let hsize = le32(&sec, 12) as usize;
    if !(92..=sec.len()).contains(&hsize) {
        return Err(PartError::Corrupt("bad GPT header size"));
    }
    let mut h = sec[..hsize].to_vec();
    let want = le32(&h, 16);
    h[16..20].fill(0);
    if crc32(&h) != want {
        return Err(PartError::Corrupt("GPT header CRC mismatch"));
    }
    if le32(&h, 8) != 0x0001_0000 {
        return Err(PartError::Corrupt("unsupported GPT revision"));
    }
    if le64(&h, 24) != hdr_lba {
        return Err(PartError::Corrupt("GPT header LBA mismatch"));
    }
    let (first_usable, last_usable) = (le64(&h, 40), le64(&h, 48));
    if first_usable < 2 || first_usable > last_usable || last_usable >= cx.size / cx.ss {
        return Err(PartError::Corrupt("bad GPT usable range"));
    }
    let ent_lba = le64(&h, 72);
    let count = le32(&h, 80);
    let esize = le32(&h, 84);
    if count > MAX_GPT_ENTRIES || esize < 128 || !esize.is_multiple_of(8) || esize > MAX_GPT_ESIZE {
        return Err(PartError::Corrupt("bad GPT entry array geometry"));
    }
    let total = count as u64 * esize as u64; // <= 4 MiB
    let off = ent_lba.checked_mul(cx.ss).ok_or(PartError::Corrupt("lba overflow"))?;
    if off.checked_add(total).is_none_or(|e| e > cx.size) {
        return Err(PartError::Corrupt("GPT entry array beyond device"));
    }
    let mut arr = vec![0u8; total as usize];
    cx.dev.read_at(off, &mut arr)?;
    if crc32(&arr) != le32(&h, 88) {
        return Err(PartError::Corrupt("GPT entry array CRC mismatch"));
    }
    let mut out = Vec::new();
    for i in 0..count as usize {
        let e = &arr[i * esize as usize..(i + 1) * esize as usize];
        let mut guid = [0u8; 16];
        guid.copy_from_slice(&e[0..16]);
        if guid == [0u8; 16] {
            continue;
        }
        let (first, lastl) = (le64(e, 32), le64(e, 40));
        let sectors = lastl
            .checked_sub(first)
            .and_then(|d| d.checked_add(1))
            .ok_or(PartError::Corrupt("bad GPT entry range"))?;
        if first < first_usable || lastl > last_usable {
            return Err(PartError::Corrupt("GPT entry outside usable range"));
        }
        let (start, len) = cx.range(first, sectors)?;
        let units: Vec<u16> = e[56..128]
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| u16::from_le_bytes(*c))
            .take_while(|&u| u != 0)
            .collect();
        out.push(PartitionInfo {
            index: i as u32 + 1,
            start,
            len,
            kind: PartKind::Gpt { type_guid: guid, name: String::from_utf16_lossy(&units) },
        });
    }
    check_overlap(out.iter().map(|p| (p.start, p.len)).collect())?;
    Ok(out)
}

/// A `BlockDevice` window onto one partition.
pub struct PartitionView {
    dev: Arc<dyn BlockDevice>,
    start: u64,
    len: u64,
}

impl PartitionView {
    pub fn new(dev: Arc<dyn BlockDevice>, info: &PartitionInfo) -> Result<PartitionView, Errno> {
        match info.start.checked_add(info.len) {
            Some(end) if end <= dev.size() => {
                Ok(PartitionView { dev, start: info.start, len: info.len })
            }
            _ => Err(Errno::EINVAL),
        }
    }

    fn translate(&self, off: u64, n: usize) -> Result<u64, Errno> {
        match off.checked_add(n as u64) {
            Some(end) if end <= self.len => Ok(self.start + off),
            _ => Err(Errno::EINVAL),
        }
    }
}

impl BlockDevice for PartitionView {
    fn size(&self) -> u64 {
        self.len
    }
    fn sector_size(&self) -> u32 {
        self.dev.sector_size()
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> Result<(), Errno> {
        let o = self.translate(off, buf.len())?;
        self.dev.read_at(o, buf)
    }
    fn write_at(&self, off: u64, buf: &[u8]) -> Result<(), Errno> {
        let o = self.translate(off, buf.len())?;
        self.dev.write_at(o, buf)
    }
    fn flush(&self) -> Result<(), Errno> {
        self.dev.flush()
    }
    fn read_only(&self) -> bool {
        self.dev.read_only()
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn crc32_known_vector() {
        assert_eq!(super::crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(super::crc32(b""), 0);
    }
}
