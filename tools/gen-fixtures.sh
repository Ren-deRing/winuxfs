#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Generate the XFS test images (M2 design 10.2-10.4). Unprivileged: images are
# sparse files filled by `mkfs.xfs -p <dir>`; nothing is mounted.
#
# Usage: tools/gen-fixtures.sh [--out DIR] [--self-test] [--list]
#   --out DIR     output directory (default tests/xfs/images, git-ignored)
#   --self-test   build ONE small image plus the small hole/unwritten image in a
#                 temp dir, check each with xfs_repair -n, and exit
#   --list        print the matrix and exit
# Exit code 77 (with an install hint) when a needed tool is missing.
#
# Per image NAME: NAME.img [NAME.log.img] [NAME.rt.img], NAME.manifest.json
# (tools/xfs_manifest.py of the source tree) and NAME.expect.json (superblock,
# root ino, fork format of every selected path, from xfs_db).
set -euo pipefail
cd "$(dirname "$0")/.."
HERE=$PWD
MANIFEST="$HERE/tools/xfs_manifest.py"

OUT=tests/xfs/images
SELF=0
LIST=0
while [ $# -gt 0 ]; do
  case $1 in
    --out) OUT=${2:?--out needs a directory}; shift 2 ;;
    --self-test) SELF=1; shift ;;
    --list) LIST=1; shift ;;
    *) echo "usage: $0 [--out DIR] [--self-test] [--list]" >&2; exit 2 ;;
  esac
done

need=()
for t in mkfs.xfs xfs_db xfs_repair xfs_logprint truncate python3; do
  command -v "$t" >/dev/null || need+=("$t")
done
if [ ${#need[@]} -gt 0 ]; then
  echo "SKIP: missing tools: ${need[*]}" >&2
  echo "install xfsprogs (Fedora: dnf install xfsprogs; Debian/Ubuntu: apt install xfsprogs) and python3" >&2
  exit 77
fi

# ---------------------------------------------------------------- matrix ---
# Orthogonal array: 16 runs of the Hadamard array L16(2^15). Run i, column c
# has level parity(i & c). Factors and their columns:
#   block size   cols 1,2: 00->1024  01->4096  10->65536  11->4096
#   reflink 3, sector 4 (0:512 1:4096), rmapbt 5, bigtime 6, metadir 7,
#   dir block 8 (0:default 1:64K), nrext64 9, external log 10, RT device 12.
# Any two distinct columns are balanced, so every pair of binary factors
# meets in all four combinations. Sector 4096 with block size 1024 is
# impossible (mkfs: block size cannot be smaller than the sector) and is
# clamped to 512 for those runs. RT runs with reflink or rmapbt are promoted
# to metadir=1 (mkfs refuses them otherwise). See tests/xfs/README.md.
par() { local v=$(( $1 & $2 )) p=0; while [ "$v" -gt 0 ]; do p=$(( p ^ (v & 1) )); v=$(( v >> 1 )); done; echo "$p"; }

matrix_row() {  # i -> sets BS SS DIRBLK RFLK RMAP BIG NR64 XLOG RT MDIR NAME
  local i=$1 b
  b=$(( $(par "$i" 1) * 2 + $(par "$i" 2) ))
  case $b in 0) BS=1024 ;; 1|3) BS=4096 ;; 2) BS=65536 ;; esac
  SS=$(( $(par "$i" 4) == 1 ? 4096 : 512 ))
  [ "$SS" -le "$BS" ] || SS=512
  DIRBLK=$(( $(par "$i" 8) == 1 ? 65536 : 0 ))
  RFLK=$(par "$i" 3); RMAP=$(par "$i" 5); BIG=$(par "$i" 6); MDIR=$(par "$i" 7)
  NR64=$(par "$i" 9); XLOG=$(par "$i" 10); RT=$(par "$i" 12)
  # mkfs: "reflink not supported on realtime devices without metadir" (same
  # for rmapbt), so RT runs that want either get metadir=1.
  if [ "$RT" = 1 ] && [ "$MDIR" = 0 ] && [ $(( RFLK + RMAP )) -gt 0 ]; then MDIR=1; fi
  local f=""
  [ "$RFLK" = 1 ] && f+="-reflink"
  [ "$RMAP" = 1 ] && f+="-rmap"
  [ "$BIG" = 1 ] && f+="-bigtime"
  [ "$NR64" = 1 ] && f+="-nrext64"
  [ "$MDIR" = 1 ] && f+="-metadir"
  [ "$XLOG" = 1 ] && f+="-xlog"
  [ "$RT" = 1 ] && f+="-rt"
  [ -n "$f" ] || f="-plain"
  NAME=$(printf 'm%02d-b%s-s%s-d%s%s' "$i" "$BS" "$SS" \
         "$([ "$DIRBLK" = 0 ] && echo def || echo 64k)" "$f")
}

if [ "$LIST" = 1 ]; then
  for i in $(seq 0 15); do matrix_row "$i"; echo "$NAME"; done
  exit 0
fi

# ------------------------------------------------------- source trees (py) ---
# usage: gen-src KIND DIR BLOCKSIZE DIRBLOCKSIZE PROFILE   (KIND=main|hole)
# main: the tree shared by every matrix image (file content depends on the
# block size so "one block" really is one block). PROFILE=small trims the
# big pieces for --self-test. hole: the tree of the xfs_db-edited image; DIR
# gets the tree as written and DIR.exp what a reader must see.
read -r -d '' PY_SRC <<'PY' || true
import os, random, sys
kind, root, bs, dbs, profile = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
small = profile == "small"
rng = random.Random(0x58465321)
# mkfs -p copies data in units of the page size: holes shorter than 4096
# bytes are not preserved, so hole/island geometry uses unit = max(bs, 4096).
unit = max(bs, 4096)

def mk(path, data=b"", mode=None):
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full) or root, exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)
    if mode is not None:
        os.chmod(full, mode)

def islands(path, count, stride, size=16):
    """count one-block islands separated by holes: count separate extents."""
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        for i in range(count):
            f.seek(i * stride * unit)
            f.write(b"i%d:" % i + rng.randbytes(size))

def main():
    mk("empty.txt")
    mk("tiny.txt", b"hello xfs\n")
    mk("one_byte.bin", b"\x01")
    mk("one_block.bin", rng.randbytes(bs))
    mk("block_plus_1.bin", rng.randbytes(bs + 1))
    mk("multi_block.bin", rng.randbytes(3 * bs + 17))
    mk("modes/exec.sh", b"#!/bin/sh\n", 0o755)
    mk("modes/readonly.txt", b"ro\n", 0o444)
    mk("modes/private.txt", b"p\n", 0o600)
    mk("deep/a/b/c/d/e/f/leaf.txt", b"deep\n")
    # fork formats: extents, btree with a one-level and a two-level tree
    islands("extents/frag8.bin", 8, 2)
    islands("extents/bmbt_leaf.bin", 64, 2)
    # The root of the tree in the inode holds ~20 keys and a leaf block
    # hundreds of records: two levels need more extents than that. At 64 KiB
    # blocks the data would not fit in the image, so there the file only
    # reaches one level (see tests/xfs/README.md).
    n2 = {1024: 2000, 4096: 6000}.get(bs, 400)
    if small:
        n2 = min(n2, 100)
    islands("extents/bmbt_deep.bin", n2, 2)
    # holes (mkfs -p preserves them)
    os.makedirs(os.path.join(root, "sparse"))
    with open(os.path.join(root, "sparse/head_hole.bin"), "wb") as f:
        f.seek(3 * unit + 5); f.write(b"after-head-hole")
    with open(os.path.join(root, "sparse/tail_hole.bin"), "wb") as f:
        f.write(b"before-tail-hole"); f.truncate(1 << 28)
    with open(os.path.join(root, "sparse/mid_hole.bin"), "wb") as f:
        f.write(b"A" * bs); f.seek(5 * unit); f.write(b"B" * bs + b"tail")
    if not small:
        with open(os.path.join(root, "sparse/big_gt4g.bin"), "wb") as f:
            f.write(b"start"); f.seek((4 << 30) + 4096 + 3); f.write(b"end>4GiB")
    # directories of each on-disk form (shortform, block, leaf, node)
    for i in range(3):
        mk("dir_sf/f%d" % i)
    for i in range(max(10, dbs // 100)):
        mk("dir_block/file_%04d" % i)
    for i in range(2 * dbs // 80):
        mk("dir_leaf/%s_%05d" % ("L" * 50, i))
    os.makedirs(os.path.join(root, "many"))
    for i in range(300 if small else 3000):
        open(os.path.join(root, "many", "entry_%05d.dat" % i), "wb").close()
    if dbs >= 65536 and not small:
        # 3000 short names still fit one 64 KiB directory block
        os.makedirs(os.path.join(root, "many_xl"))
        for i in range(20000):
            open(os.path.join(root, "many_xl", "entry_%05d.dat" % i), "wb").close()
    # names
    mk("names/한글파일.txt", "한글 이름\n".encode())
    mk("names/디렉터리/파일", b"k\n")
    mk("names/" + "a" * 255, b"255 ascii\n")
    mk("names/" + "가" * 85, b"255 bytes of korean\n")   # 85 * 3 = 255 bytes
    mk("names/.hidden.multi.dot.", b"s\n")
    fd = os.open(os.path.join(root, "names").encode() + b"/non_utf8_caf\xe9",
                 os.O_CREAT | os.O_WRONLY, 0o644)
    os.write(fd, b"latin1\n")
    os.close(fd)
    # links, fifo
    os.symlink("tiny.txt", os.path.join(root, "links_local"))
    os.symlink("./" * 146 + "tiny.txt", os.path.join(root, "links_remote_300"))   # 300 bytes
    os.symlink("no/such/target", os.path.join(root, "links_dangling"))
    os.makedirs(os.path.join(root, "hard"))
    os.link(os.path.join(root, "tiny.txt"), os.path.join(root, "hard/tiny.link"))
    os.link(os.path.join(root, "multi_block.bin"), os.path.join(root, "hard/multi.link1"))
    os.link(os.path.join(root, "multi_block.bin"), os.path.join(root, "hard/multi.link2"))
    os.mkfifo(os.path.join(root, "fifo"))

def hole_main():
    # bs is 4096. root = as written; root.exp = what a reader must see.
    exp = root + ".exp"
    def both(path, blocks, unwritten=()):
        n = max(blocks) + 1
        for base, z in ((root, False), (exp, True)):
            full = os.path.join(base, path)
            os.makedirs(os.path.dirname(full) or base, exist_ok=True)
            with open(full, "wb") as f:
                f.truncate(n * bs)
                for b, v in sorted(blocks.items()):
                    if z and b in unwritten:
                        continue      # unwritten extents read as zeros
                    f.seek(b * bs); f.write(bytes([v]) * bs)
    both("isl.bin", {0: 0x41, 2: 0x42, 4: 0x43}, unwritten=(2,))
    both("whole.bin", {0: 0x57, 1: 0x57, 2: 0x57, 3: 0x57}, unwritten=(0, 1, 2, 3))
    both("hole.bin", {0: 0x41, 3: 0x42})                      # native holes
    for base in (root, exp):
        with open(os.path.join(base, "hole.bin"), "r+b") as f:
            f.truncate(8 * bs)                                 # trailing hole
        with open(os.path.join(base, "far.bin"), "wb") as f:   # offset 1<<40
            f.write(b"near"); f.seek(1 << 40); f.write(b"far")
        with open(os.path.join(base, "tiny.txt"), "wb") as f:
            f.write(b"hello xfs\n")

if kind == "main":
    main()
else:
    hole_main()
PY

# ------------------------------------------------------ expected values (py) ---
# usage: gen-expect IMG MANIFEST LOGIMG|- RTIMG|-  -> JSON on stdout
read -r -d '' PY_EXPECT <<'PY' || true
import json, re, subprocess, sys
img, manifest, logimg, rtimg = sys.argv[1:5]
opts = ["-r"] + (["-l", logimg] if logimg != "-" else []) + (["-R", rtimg] if rtimg != "-" else [])

def db(*cmds):
    argv = ["xfs_db"] + opts
    for c in cmds:
        argv += ["-c", c]
    r = subprocess.run(argv + [img], capture_output=True)
    if r.returncode != 0:
        sys.exit("xfs_db failed: %r: %s" % (cmds, r.stderr.decode(errors="replace")))
    return r.stdout.decode("utf-8", "surrogateescape")

def val(s):
    s = re.sub(r" \((correct|btree|extents|local|dev|uuid)\)$", "", s.strip())
    if re.fullmatch(r"-?\d+", s):
        return int(s)
    if re.fullmatch(r"0x[0-9a-fA-F]+", s):
        return int(s, 16)
    return s.strip('"')

def fields(text):
    d = {}
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z0-9_.\[\]-]+) = (.*)$", line)
        if m:
            d[m.group(1)] = (m.group(2), val(m.group(2)))
    return d

sb = {k: v[1] for k, v in fields(db("sb 0", "p")).items() if k not in ("fname", "crc")}
lp = ["xfs_logprint", "-t"] + (["-l", logimg] if logimg != "-" else []) + [img]
m = re.search(r"state: <(\w+)>", subprocess.run(lp, capture_output=True, text=True).stdout)
bs = sb["blocksize"]
dbs = bs << sb["dirblklog"]

def bmap(p):
    """[(startoff, count, flag)] of path p, in file system blocks."""
    out = []
    for line in db("path " + p, "bmap").splitlines():
        mm = re.search(r"offset (\d+) startblock \d+ \(\d+/\d+\) count (\d+)(?: flag (\d+))?", line)
        if mm:
            out.append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3) or 0)))
    return out

def dirform(fmt, p):
    if fmt == "local":
        return "shortform"
    leaf = sum(c for o, c, _ in bmap(p) if o * bs >= 1 << 35)   # leaf space starts at 32 GiB
    if leaf == 0:
        return "block"
    return "leaf" if leaf <= dbs // bs else "node"

paths = {}
for p, e in json.load(open(manifest))["entries"].items():
    # all paths except the bulk of the big directories
    if re.match(r"^/many(_xl)?/entry_", p) and not p.endswith(("entry_00000.dat", "entry_00001.dat")):
        continue
    f = fields(db("path " + p, "inode", "p"))
    if "v3.inumber" not in f:
        sys.exit("no v3.inumber for %r: %r" % (p, sorted(f)[:8]))
    d = {"kind": e["kind"], "ino": f["v3.inumber"][1],
         "mode_octal": f["core.mode"][0], "size": f["core.size"][1],
         "format": re.sub(r"^\d+ \((\w+)\)$", r"\1", f["core.format"][0]),
         "nextents": f["core.nextents"][1], "nblocks": f["core.nblocks"][1],
         "nlink": f["core.nlinkv2"][1], "flags2": f["v3.flags2"][1]}
    if "u3.bmbt.level" in f:
        d["bmbt_level"] = f["u3.bmbt.level"][1]
    if e["kind"] == "dir":
        d["dir_form"] = dirform(d["format"], p)
    elif e["kind"] == "file" and d["nextents"] <= 64:
        d["extents"] = [{"startoff": o, "count": c, "unwritten": bool(fl)} for o, c, fl in bmap(p)]
    paths[p] = d
print(json.dumps({"superblock": sb, "root_ino": sb["rootino"],
                  "log_state": m.group(1) if m else "UNKNOWN",
                  "paths": paths}, indent=1, sort_keys=True))
PY

# --------------------------------------------------------------- helpers ---
WORK=
cleanup() { if [ -n "$WORK" ] && [ -d "$WORK" ]; then rm -rf -- "${WORK:?}"; fi; return 0; }
trap cleanup EXIT

run_repair() {  # IMG LOG RT NAME -> fail unless xfs_repair -n is clean
  local img=$1 log=$2 rt=$3 name=$4 args=(-n) rc=0 outf
  [ "$log" != - ] && args+=(-l "$log")
  [ "$rt" != - ] && args+=(-r "$rt")
  outf=$WORK/repair.$name.txt
  xfs_repair "${args[@]}" "$img" >"$outf" 2>&1 || rc=$?
  if [ "$rc" != 0 ] || grep -q -E 'Incorrect|corrupt|bad |would (fix|have)|inconsistent|dangling' "$outf"; then
    echo "FAIL: xfs_repair -n not clean for $name (rc=$rc):" >&2
    grep -v -E '^(Phase|        - )' "$outf" >&2
    exit 1
  fi
}

shrink() {  # re-sparse zero runs left by mkfs (log zeroing); best effort
  command -v fallocate >/dev/null || return 0
  fallocate --dig-holes "$@" 2>/dev/null || true
}

make_src() {  # KIND BS DBS PROFILE DIR
  mkdir -p "$5"
  python3 -I -c "$PY_SRC" "$1" "$5" "$2" "$3" "$4"
}

# build_image NAME BS SS DIRBLK RFLK RMAP BIG NR64 XLOG RT MDIR PROFILE
build_image() {
  local name=$1 bs=$2 ss=$3 dirblk=$4 rflk=$5 rmap=$6 big=$7 nr64=$8 xlog=$9 rt=${10} mdir=${11} profile=${12}
  local dbs=$(( dirblk == 0 ? (bs < 4096 ? 4096 : bs) : dirblk ))   # mkfs default dir block: max(bs, 4096)
  local src=$WORK/src.$bs.$dbs.$profile
  local img="${OUT:?}/${name:?}.img" log=- rtd=-
  if [ ! -d "$src" ]; then
    make_src main "$bs" "$dbs" "$profile" "$src"
    python3 -I "$MANIFEST" dir --skip-hash-over $(( 1 << 40 )) "$src" > "$src.manifest.json"
  fi
  local mk=(mkfs.xfs -q -f -b size="$bs" -s size="$ss"
            -m "crc=1,reflink=$rflk,rmapbt=$rmap,bigtime=$big,metadir=$mdir"
            -i "nrext64=$nr64")
  [ "$dirblk" != 0 ] && mk+=(-n size="$dirblk")
  rm -f -- "$img" "${OUT:?}/${name:?}.log.img" "${OUT:?}/${name:?}.rt.img"
  truncate -s 300M "$img"
  local sh=("$img")
  if [ "$xlog" = 1 ]; then
    log="${OUT:?}/${name:?}.log.img"; truncate -s 100M "$log"
    mk+=(-l "logdev=$log,size=64m"); sh+=("$log")
  fi
  if [ "$rt" = 1 ]; then
    rtd="${OUT:?}/${name:?}.rt.img"; truncate -s 64M "$rtd"
    mk+=(-r "rtdev=$rtd"); sh+=("$rtd")
  fi
  "${mk[@]}" -p "$src" "$img" || { echo "FAIL: mkfs.xfs for $name: ${mk[*]}" >&2; exit 1; }
  run_repair "$img" "$log" "$rtd" "$name"
  local lp=(xfs_logprint -t); [ "$log" != - ] && lp+=(-l "$log")
  "${lp[@]}" "$img" 2>&1 | grep -q 'state: <CLEAN>' || { echo "FAIL: log of $name not CLEAN" >&2; exit 1; }
  cp -- "$src.manifest.json" "$OUT/$name.manifest.json"
  python3 -I -c "$PY_EXPECT" "$img" "$OUT/$name.manifest.json" "$log" "$rtd" > "$OUT/$name.expect.json"
  shrink "${sh[@]}"
  echo "ok: $name"
}

# Small image with holes, unwritten extents and a 1 TiB sparse offset. The
# unwritten flags cannot come from mkfs -p, so they are set with xfs_db -x on
# the otherwise ordinary image (documented in tests/xfs/README.md):
#   xfs_db -x -c 'path /isl.bin'   -c 'write u3.bmx[1].extentflag 1' IMG   # block 2 of 0,2,4
#   xfs_db -x -c 'path /whole.bin' -c 'write u3.bmx[0].extentflag 1' IMG   # all 4 blocks
# rmapbt=0 and reflink=0: with rmapbt xfs_repair -n reports "Incorrect
# reverse-mapping ... should be ... unwritten" for these two files because the
# rmap records were written before the flags were flipped. Without rmapbt the
# image is clean, so no warning has to be tolerated.
build_hole_image() {
  local name=hole-unwritten
  local img="${OUT:?}/hole-unwritten.img" src=$WORK/src.hole
  make_src hole 4096 4096 main "$src"
  python3 -I "$MANIFEST" dir --skip-hash-over $(( 1 << 30 )) "$src.exp" > "$OUT/$name.manifest.json"
  rm -f -- "$img"
  truncate -s 300M "$img"
  mkfs.xfs -q -f -b size=4096 -m crc=1,reflink=0,rmapbt=0 -p "$src" "$img"
  xfs_db -x -c 'path /isl.bin' -c 'write u3.bmx[1].extentflag 1' "$img" >/dev/null
  xfs_db -x -c 'path /whole.bin' -c 'write u3.bmx[0].extentflag 1' "$img" >/dev/null
  run_repair "$img" - - "$name"
  python3 -I -c "$PY_EXPECT" "$img" "$OUT/$name.manifest.json" - - > "$OUT/$name.expect.json"
  shrink "$img"
  echo "ok: $name"
}

# ------------------------------------------------------------------ main ---
if [ "$SELF" = 1 ]; then
  OUT=$(mktemp -d "${TMPDIR:-/tmp}/wfs-xfs-selftest.XXXXXX")
  WORK=$OUT          # one temp directory, removed on exit
  build_image selftest 4096 512 0 1 1 1 1 0 0 0 small
  build_hole_image
  echo "self-test OK"
  exit 0
fi

mkdir -p "$OUT"
WORK=$(mktemp -d "$OUT/.work.XXXXXX")
echo "writing $OUT"
for i in $(seq 0 15); do
  matrix_row "$i"
  build_image "$NAME" "$BS" "$SS" "$DIRBLK" "$RFLK" "$RMAP" "$BIG" "$NR64" "$XLOG" "$RT" "$MDIR" main
done
build_hole_image
echo "done: $(ls "$OUT"/*.img | wc -l) image files in $OUT"
