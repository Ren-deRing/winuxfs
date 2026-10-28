#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Fixtures that need the Linux kernel (mount, fallocate, reflink, mknod, a
# dirty log). Run ONCE, by a person, as root, on a Linux host with loop
# devices; commit the resulting tests/xfs/root-fixtures/*.img.zst. It is never
# run by tools/check.sh and never by an agent.
#
# Usage: sudo tools/gen-root-fixtures.sh [--yes] [--out DIR]
#   --yes      do not wait for confirmation
#   --out DIR  output directory (default tests/xfs/root-fixtures)
#
# Safety: refuses to run unless root; touches only files it creates in one
# fresh mktemp directory and the loop devices it attaches to those files
# (never an existing device); on any exit it unmounts, detaches its own loop
# devices and removes the temp directory.
#
# Images produced (each compressed with zstd as NAME.img.zst; the external
# log and RT images of the last two are NAME.log.img.zst / NAME.rt.img.zst):
#   sparse        holes written by the kernel (fallocate punch, truncate)
#   unwritten     preallocated (fallocate) unwritten extents, partly written
#   fragmented    file written in an interleaved pattern: many small extents
#   reflink       cp --reflink: shared extents (reflink=1,rmapbt=1)
#   devnodes      character, block device nodes, socket, fifo
#   dirty-log     metadata changes, then `xfs_io -x -c "shutdown -f"` and
#                 unmount: the log keeps transactions (xfs_logprint -t shows
#                 <DIRTY>)
#   extlog-rt     external log + realtime device, files on the RT device
#                 (chattr +t via xfs_io) written through the kernel
#   rt-extsize    like extlog-rt with rextsize 4 blocks, unaligned file sizes
set -euo pipefail

OUT_REL=tests/xfs/root-fixtures
YES=0
while [ $# -gt 0 ]; do
  case $1 in
    --yes) YES=1; shift ;;
    --out) OUT_REL=${2:?--out needs a directory}; shift 2 ;;
    *) echo "usage: $0 [--yes] [--out DIR]" >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" != 0 ]; then
  echo "refusing to run: this script must be run as root (it mounts loop devices)." >&2
  echo "Run it once on a trusted Linux host:  sudo $0" >&2
  exit 1
fi

for t in losetup mount umount mkfs.xfs xfs_io xfs_logprint zstd truncate cp mknod; do
  command -v "$t" >/dev/null || { echo "SKIP: missing tool: $t (install xfsprogs, util-linux, zstd)" >&2; exit 77; }
done

REPO=$(cd "$(dirname "$0")/.." && pwd)
case $OUT_REL in /*) OUT=$OUT_REL ;; *) OUT=$REPO/$OUT_REL ;; esac

cat <<EOF
gen-root-fixtures.sh will:
  - create one temporary directory under \${TMPDIR:-/tmp}
  - create sparse image files in it (about 300 MiB apparent each, a few MiB used)
  - attach each to a NEW loop device (losetup --find --show), mkfs.xfs it,
    mount it under that directory, write the fixture content, unmount, detach
  - for 'dirty-log': freeze, copy the raw image while the log is not
    checkpointed, thaw; the copy is the fixture
  - compress the images with zstd into: $OUT
  - on exit (success or failure): unmount, detach only its own loop devices,
    delete the temporary directory
It never touches any existing block device or file system.
EOF
if [ "$YES" != 1 ]; then
  read -r -p "Proceed? [y/N] " ans
  [ "$ans" = y ] || [ "$ans" = Y ] || { echo "aborted"; exit 1; }
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/wfs-root-fixtures.XXXXXX")
LOOPS=()      # loop devices created by this script
MOUNTS=()     # mount points created by this script
cleanup() {
  local m l
  set +e
  for m in "${MOUNTS[@]:-}"; do [ -n "$m" ] && mountpoint -q "$m" && umount "$m"; done
  for l in "${LOOPS[@]:-}"; do [ -n "$l" ] && losetup -d "$l"; done
  [ -n "${TMP:-}" ] && [ -d "$TMP" ] && rm -rf -- "${TMP:?}"
  return 0
}
trap cleanup EXIT

mkdir -p "$OUT"

# attach IMG -> prints the new loop device (registered for cleanup)
attach() {
  local dev
  dev=$(losetup --find --show "$1")
  LOOPS+=("$dev")
  echo "$dev"
}
detach() {  # DEV: detach now and forget it
  local d=$1 i
  losetup -d "$d"
  for i in "${!LOOPS[@]}"; do [ "${LOOPS[$i]}" = "$d" ] && LOOPS[$i]=""; done
}
mnt() {  # DEV MOUNTPOINT [extra mount options]
  mkdir -p "$2"
  mount -t xfs ${3:+-o "$3"} "$1" "$2"
  MOUNTS+=("$2")
}
umnt() {
  umount "$1"
  local i
  for i in "${!MOUNTS[@]}"; do [ "${MOUNTS[$i]}" = "$1" ] && MOUNTS[$i]=""; done
}
finish() {  # NAME IMG: compress into OUT
  zstd -q -19 -f -o "$OUT/$1.img.zst" "$2"
  echo "wrote $OUT/$1.img.zst"
}
newimg() { truncate -s "$2" "$TMP/$1.img"; echo "$TMP/$1.img"; }

# ---- sparse + unwritten + fragmented (one data volume each) -----------------
img=$(newimg sparse 300M); dev=$(attach "$img")
mkfs.xfs -q -f -m reflink=0,rmapbt=0 "$dev"
mnt "$dev" "$TMP/m"
xfs_io -f -c "pwrite -S 0x41 0 1m" -c "fpunch 256k 512k" "$TMP/m/punched.bin" >/dev/null
xfs_io -f -c "pwrite -S 0x42 0 4k" -c "pwrite -S 0x43 1g 4k" "$TMP/m/gap_1g.bin" >/dev/null
xfs_io -f -c "pwrite 0 4k" -c "truncate 64m" "$TMP/m/tail_hole.bin" >/dev/null
sync; umnt "$TMP/m"; detach "$dev"; finish sparse "$img"

img=$(newimg unwritten 300M); dev=$(attach "$img")
mkfs.xfs -q -f -m reflink=0,rmapbt=0 "$dev"
mnt "$dev" "$TMP/m"
xfs_io -f -c "falloc 0 1m" "$TMP/m/all_unwritten.bin" >/dev/null
xfs_io -f -c "falloc 0 1m" -c "pwrite -S 0x44 256k 4k" "$TMP/m/partly_written.bin" >/dev/null
sync; umnt "$TMP/m"; detach "$dev"; finish unwritten "$img"

img=$(newimg fragmented 300M); dev=$(attach "$img")
mkfs.xfs -q -f -m reflink=0,rmapbt=0 "$dev"
mnt "$dev" "$TMP/m"
# two files written alternately block by block interleave their extents
for i in $(seq 0 511); do
  xfs_io -f -c "pwrite -S 0x45 $((i * 4096)) 4096" "$TMP/m/frag_a.bin" >/dev/null
  xfs_io -f -c "pwrite -S 0x46 $((i * 4096)) 4096" "$TMP/m/frag_b.bin" >/dev/null
  sync -f "$TMP/m"
done
sync; umnt "$TMP/m"; detach "$dev"; finish fragmented "$img"

# ---- reflink -----------------------------------------------------------------
img=$(newimg reflink 300M); dev=$(attach "$img")
mkfs.xfs -q -f -m reflink=1,rmapbt=1 "$dev"
mnt "$dev" "$TMP/m"
xfs_io -f -c "pwrite -S 0x47 0 256k" "$TMP/m/orig.bin" >/dev/null
cp --reflink=always "$TMP/m/orig.bin" "$TMP/m/clone.bin"
xfs_io -c "pwrite -S 0x48 64k 4k" "$TMP/m/clone.bin" >/dev/null   # partial CoW
sync; umnt "$TMP/m"; detach "$dev"; finish reflink "$img"

# ---- device nodes and special files --------------------------------------------
img=$(newimg devnodes 300M); dev=$(attach "$img")
mkfs.xfs -q -f "$dev"
mnt "$dev" "$TMP/m"
mknod "$TMP/m/chr_1_3" c 1 3
mknod "$TMP/m/blk_7_0" b 7 0
mknod "$TMP/m/fifo" p
python3 -I -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])" "$TMP/m/sock"
sync; umnt "$TMP/m"; detach "$dev"; finish devnodes "$img"

# ---- dirty log -----------------------------------------------------------------
# `shutdown -f` forces the log to disk and stops the file system without
# checkpointing; unmounting a shut-down file system writes no unmount record,
# so the log keeps transactions that only a replay applies.
img=$(newimg dirty 300M); dev=$(attach "$img")
mkfs.xfs -q -f "$dev"
mnt "$dev" "$TMP/m"
xfs_io -f -c "pwrite -S 0x49 0 64k" "$TMP/m/before.bin" >/dev/null
sync
for i in $(seq 0 199); do : > "$TMP/m/new_$i"; done
mkdir "$TMP/m/new_dir"
xfs_io -x -c "shutdown -f" "$TMP/m"
umnt "$TMP/m"
if xfs_logprint -t "$dev" 2>&1 | grep -q 'state: <DIRTY>'; then
  detach "$dev"; finish dirty-log "$img"
else
  echo "ERROR: log of dirty-log image is not DIRTY; not writing it" >&2
  exit 1
fi

# ---- external log + realtime device -----------------------------------------------
for variant in extlog-rt rt-extsize; do
  img=$(newimg "$variant" 300M); logimg=$(newimg "$variant.log" 100M); rtimg=$(newimg "$variant.rt" 200M)
  dev=$(attach "$img"); logdev=$(attach "$logimg"); rtdev=$(attach "$rtimg")
  extsz=1; [ "$variant" = rt-extsize ] && extsz=16k
  mkfs.xfs -q -f -m reflink=0,rmapbt=0 -l logdev="$logdev",size=64m -r rtdev="$rtdev",extsize=$extsz "$dev"
  mnt "$dev" "$TMP/m" "logdev=$logdev,rtdev=$rtdev"
  mkdir "$TMP/m/rt"
  xfs_io -c "chattr +t" "$TMP/m/rt"          # files created below inherit realtime
  xfs_io -f -c "pwrite -S 0x4a 0 1m" "$TMP/m/rt/one_meg.bin" >/dev/null
  xfs_io -f -c "pwrite -S 0x4b 0 5000" "$TMP/m/rt/unaligned_5000.bin" >/dev/null
  xfs_io -f -c "pwrite -S 0x4c 0 4k" -c "pwrite -S 0x4d 8m 4k" "$TMP/m/rt/sparse.bin" >/dev/null
  xfs_io -f -c "pwrite -S 0x4e 0 4k" "$TMP/m/data_dev.bin" >/dev/null   # a non-RT file too
  sync; umnt "$TMP/m"; detach "$dev"; detach "$logdev"; detach "$rtdev"
  finish "$variant" "$img"; finish "$variant.log" "$logimg"; finish "$variant.rt" "$rtimg"
done

echo "done. Verify: for f in $OUT/*.img.zst; do zstd -dc \$f | xfs_repair -n /dev/stdin; done  (external-device images need -l/-r)"
