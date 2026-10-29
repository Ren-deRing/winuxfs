#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Regenerates tests/xfs/corpus/ (design 10.6): base-small.wsi (the sectors the
# clean walk reads from a small image holding every directory form, extent and
# btree forks and symlinks) and the minimized bad inputs, each a few sectors
# on top of that base (.wsd).
#
# Usage: tools/gen-fuzz-corpus.sh [--extra] [MUTATE_RUNNER]   (default: built by make in backends/xfs)
#   --extra  keep the committed base-small.wsi and every other input, rebuild
#            only the inputs of the extra_deltas() group below from an image
#            materialised out of that base (no mkfs; the base is unchanged, so a
#            new input does not rewrite the whole corpus).
# Needs mkfs.xfs, xfs_db (-x), python3. Output is committed; tests do not need
# xfsprogs. bad-*: the walk must report an error code; reg-*: only no
# crash/hang/over-allocation (the answer may be success).
set -euo pipefail
cd "$(dirname "$0")/.."
for t in mkfs.xfs xfs_db python3 truncate; do
  command -v "$t" >/dev/null || { echo "SKIPPED: $t missing (install xfsprogs)" >&2; exit 77; }
done
OUT=tests/xfs/corpus
EXTRA=0
if [ "${1:-}" = "--extra" ]; then EXTRA=1; shift; fi
RUNNER=${1:-}
if [ -z "$RUNNER" ]; then
  make -s -C backends/xfs fuzz-run-test SAN=1 FUZZ_BUILD=1 OUT=../../target/xfs-shim-fz CORPUS=/nonexistent FUZZ_BASE=/nonexistent >/dev/null 2>&1 || true
  RUNNER=target/xfs-shim-fz/mutate_runner
fi
[ -x "$RUNNER" ] || { echo "no mutate_runner at $RUNNER" >&2; exit 1; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fuzzcorpus.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"
[ "$EXTRA" = 1 ] || rm -f "$OUT"/*.wsi "$OUT"/*.wsd

if [ "$EXTRA" = 0 ]; then
python3 -I - "$WORK/src" <<'PY'
import os, random, sys
root = sys.argv[1]
r = random.Random(7)
def mk(p, d=b""):
    f = os.path.join(root, p); os.makedirs(os.path.dirname(f), exist_ok=True)
    open(f, "wb").write(d)
mk("tiny.txt", b"hello xfs\n"); mk("empty")
mk("multi.bin", r.randbytes(3 * 1024 + 17))
for i in range(3): mk("dir_sf/f%d" % i, b"x")
for i in range(100): mk("dir_block/file_%03d" % i)
for i in range(100): mk("dir_leaf/%s_%03d" % ("L" * 50, i))
for i in range(720): mk("dir_node/entry_%04d" % i)
def islands(p, n, stride):
    f = os.path.join(root, p); os.makedirs(os.path.dirname(f), exist_ok=True)
    with open(f, "wb") as fh:
        for i in range(n):
            fh.seek(i * stride * 4096); fh.write(b"i%d:" % i + r.randbytes(16))
islands("ext/frag8.bin", 8, 2); islands("ext/bmbt.bin", 64, 2)
os.symlink("tiny.txt", root + "/ln_short")
os.symlink("./" * 146 + "tiny.txt", root + "/ln_remote")
os.symlink("no/such", root + "/ln_dangling")
os.makedirs(root + "/hard"); os.link(root + "/tiny.txt", root + "/hard/tiny.link")
os.mkfifo(root + "/fifo")
mk("names/한글.txt", b"k\n")
PY
truncate -s 300M "$WORK/clean.img"
mkfs.xfs -q -f -b size=1024 -p "$WORK/src" "$WORK/clean.img"
"$RUNNER" --pack "$WORK/clean.img" "$OUT/base-small.wsi"
else
# materialise the committed base: its sectors on a sparse image of the same size
python3 -I - "$OUT/base-small.wsi" "$WORK/clean.img" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"WFSIMG01"
size = struct.unpack("<Q", d[8:16])[0]
n = struct.unpack("<I", d[32:36])[0]
with open(sys.argv[2], "wb") as f:
    f.truncate(size)
    p = 36
    for _ in range(n):
        role, no = struct.unpack("<IQ", d[p:p + 12])
        if role == 0:
            f.seek(no * 512); f.write(d[p + 12:p + 524])
        p += 524
PY
fi

# delta NAME CMD...: copy the clean image, run xfs_db -x with CMD (each a -c
# argument), store the sectors that differ as NAME.wsd.
delta() {
  local name=$1; shift
  local img=$WORK/$name.img args=()
  cp --sparse=always "$WORK/clean.img" "$img"
  for c in "$@"; do args+=(-c "$c"); done
  xfs_db -x "${args[@]}" "$img" >"$WORK/$name.log" 2>&1 || true
  # xfs_db prints these for a wrong field name or index: a mistake in this script
  if grep -qiE 'not found|out of range|no such|unknown|usage' "$WORK/$name.log"; then
    echo "FAIL: $name: $(head -3 "$WORK/$name.log")" >&2; exit 1
  fi
  python3 -I - "$WORK/clean.img" "$img" "$OUT/$name.wsd" base-small.wsi <<'PY'
import struct, sys
a, b, out, base = sys.argv[1:5]
SEC = 512
secs = []
with open(a, "rb") as fa, open(b, "rb") as fb:
    off = 0
    while True:
        x = fa.read(1 << 20); y = fb.read(1 << 20)
        if not x and not y: break
        if x != y:
            for i in range(0, len(y), SEC):
                if x[i:i+SEC] != y[i:i+SEC]:
                    secs.append(((off + i) // SEC, y[i:i+SEC]))
        off += len(y)
if not secs: sys.exit("no difference: edit had no effect")
with open(out, "wb") as f:
    f.write(b"WFSDLT01"); f.write(base.encode().ljust(96, b"\0"))
    f.write(struct.pack("<I", len(secs)))
    for no, d in secs:
        f.write(struct.pack("<IQ", 0, no)); f.write(d.ljust(SEC, b"\0"))
print("%-40s %d sector(s)" % (out.split("/")[-1], len(secs)))
PY
  rm -f "$img"
}

main_deltas() {
# Dir block chain / tree: a node entry that points back at its own block, a
# leafn sibling link that points at itself.
delta bad-dir-node-cycle 'path /dir_node' 'dblock 33554432' 'write -d nbtree[0].before 33554432' 'write -d nbtree[1].before 33554432'
delta reg-dir-leafn-sibling-cycle 'path /dir_node' 'dblock 33554436' 'write -d lhdr.info.hdr.forw 33554436' 'write -d lhdr.info.hdr.back 33554436'
# CRC: stale checksum after a data change (-c), on a dir block and an inode.
delta bad-crc-dir-block 'path /dir_block' 'dblock 0' 'write -c bu[3].inumber 5'
delta bad-crc-bmbt-leaf 'path /ext/bmbt.bin' 'fsblock 131103' 'type bmapbtd' 'write -c owner 1'
# Extents: out of range, overlapping, huge count.
delta bad-extent-out-of-range 'path /ext/frag8.bin' 'write -d u3.bmx[2].startblock 1099511627775'
delta reg-extent-overlap 'path /ext/frag8.bin' 'write -d u3.bmx[1].startoff 0'
delta bad-inode-nextents 'path /ext/frag8.bin' 'write -d core.nextents 100000'
delta bad-dir-extent-out-of-range 'path /dir_leaf' 'write -d u3.bmx[0].startblock 1099511627775'
# bmbt: record count, child pointer, sibling cycle.
delta bad-bmbt-numrecs 'path /ext/bmbt.bin' 'fsblock 131103' 'type bmapbtd' 'write -d numrecs 60000'
delta bad-bmbt-ptr-out-of-range 'path /ext/bmbt.bin' 'write -d u3.bmbt.ptrs[1] 1099511627775'
delta bad-bmbt-sibling-cycle 'path /ext/bmbt.bin' 'fsblock 131103' 'type bmapbtd' 'write -d rightsib 131103' 'write -d leftsib 131103'
# Shortform directory: entry count, offsets, name length.
delta bad-sf-dir-count 'path /dir_sf' 'write -d u3.sfdir3.hdr.count 200'
delta bad-sf-dir-offsets 'path /dir_sf' 'write -d u3.sfdir3.list[0].offset 8' 'write -d u3.sfdir3.list[1].offset 4096'
delta bad-sf-dir-offset-order 'path /dir_sf' 'write -d u3.sfdir3.list[1].offset 80' 'write -d u3.sfdir3.list[2].offset 72'
delta bad-sf-dir-namelen 'path /dir_sf' 'write -d u3.sfdir3.list[0].namelen 250'
# Directory entries: inode number outside the filesystem, a loop to the root.
delta bad-dirent-ino-out-of-range 'path /dir_block' 'dblock 0' 'write -d bu[3].inumber 99999999999999'
delta reg-dirent-loop-to-root 'path /dir_block' 'dblock 0' 'write -d bu[3].inumber 64'
# Leaf `address` pointing past the entry area: the data-block over-read found by review
# (db 0, offset 4088 = blocksize - 8). leaf1 and leafn lookups.
delta bad-leaf-address-block-end 'path /dir_leaf' 'dblock 33554432' 'write -d lents[3].address 511' 'write -d lents[7].address 511'
delta bad-leafn-address-block-end 'path /dir_node' 'dblock 33554436' 'write -d lents[3].address 511' 'write -d lents[9].address 511'
# Data block entries whose length fields are inconsistent: iteration must still advance or stop.
delta bad-dir-data-namelen0 'path /dir_block' 'dblock 0' 'write -d bu[3].namelen 0'
delta bad-dir-data-namelen255 'path /dir_block' 'dblock 0' 'write -d bu[3].namelen 255'
# Symlinks: remote with a hole, longer than the 1024 limit, local overflowing the literal area.
delta bad-symlink-remote-hole 'path /ln_remote' 'write -d core.size 1500'
delta bad-symlink-size-over-1024 'path /ln_remote' 'write -d core.size 4000'
delta bad-symlink-local-size 'path /ln_short' 'write -d core.size 300'
# Inode: mode with no type, giant size, bogus format.
delta bad-inode-mode 'path /tiny.txt' 'write -d core.mode 0'
delta bad-inode-format 'path /tiny.txt' 'write -d core.format 7'
delta reg-file-size-max 'path /multi.bin' 'write -d core.size 9223372036854775807'
# Superblock / AG metadata (mount level).
delta bad-sb-agcount 'sb 0' 'write -d agcount 1000000'
delta bad-agi-root 'agi 0' 'write -d root 4294967295'
delta bad-sb-dirblklog 'sb 0' 'write -d dirblklog 30'
delta bad-sb-inodelog 'sb 0' 'write -d inodelog 20'
}

# Inputs added after the base was committed; they only need sectors the clean
# walk already reads, so `--extra` can add them without touching the base.
# bmbt leaves of /ext/bmbt.bin are fsblocks 31 and 105 (rightsib 31 -> 105). Plain
# `write` / `write -d` recompute the CRC, so the checks reached are the structural ones.
extra_deltas() {
# bmbt sibling cycle 31 -> 105 -> 31 that is valid for every check but the extent order,
# with di_nextents / di_nblocks claiming far more extents than the two leaves hold
# (review I2: unbounded incore extent map). 4M stays under the inode-load ceiling,
# so the order check (patch 0003) is what stops it; 2^32 is stopped by the ceiling.
delta bad-bmbt-sibling-cycle-nextents 'path /ext/bmbt.bin' 'fsblock 105' 'type bmapbtd' 'write rightsib 31' \
  'path /ext/bmbt.bin' 'write core.nextents 4000000' 'write core.nblocks 4000255'
delta bad-bmbt-sibling-cycle-bignextents 'path /ext/bmbt.bin' 'fsblock 105' 'type bmapbtd' 'write rightsib 31' \
  'path /ext/bmbt.bin' 'write core.nextents 4294967000' 'write core.nblocks 4294967255'
# Block-form directory lookup (xfs_dir2_block.c:749 is not patched): the verifier must reject a
# leaf `address` pointing at the end of the 4096-byte block (offset 4088); write -d keeps a good CRC
# so the data-block verifier's structure check (not the CRC) has to catch it.
delta bad-block-dir-address-block-end 'path /dir_block' 'dblock 0' 'write -d bleaf[3].address 511' 'write -d bleaf[50].address 511'
}

main_deltas_run() {
  [ "$EXTRA" = 1 ] || main_deltas
  extra_deltas
}
main_deltas_run
ls "$OUT" | wc -l
