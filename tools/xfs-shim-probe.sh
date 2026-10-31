#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Syntax-check the M3 kernel files against the XFS shim and write the gap list.
#
# Usage: tools/xfs-shim-probe.sh [--pre] [OUTDIR]
#   --pre   add the probe-only preinclude (annotation macros, empty
#           <linux/iversion.h> and xfs_qm.h) so cascades do not hide the real
#           gaps; drop it once the shim provides them (M3a T2).
#   OUTDIR  logs go to OUTDIR (default target/xfs-shim-probe); the summary is
#           printed to stdout by tools/xfs_shim_needs.py.
# Top-level fs/xfs files are compiled from a copy so the shim's xfs_platform.h
# wins over the kernel one next to them (as M2 does for xfs_dir2_readdir.c).
set -euo pipefail
cd "$(dirname "$0")/.."
pre=0
[ "${1:-}" = --pre ] && { pre=1; shift; }
OUT=$(realpath -m "${1:-target/xfs-shim-probe}")
K=third_party/linux-xfs
LIBXFS="xfs_defer xfs_trans_resv xfs_trans_space xfs_log_rlimit xfs_trans_inode
xfs_ag_resv xfs_btree_staging xfs_rmap xfs_refcount xfs_attr xfs_attr_remote"
TOP="xfs_log xfs_log_cil xfs_trans xfs_trans_buf xfs_trans_ail xfs_buf_item
xfs_buf_item_recover xfs_inode_item xfs_inode_item_recover xfs_icreate_item
xfs_iunlink_item xfs_extfree_item xfs_extent_busy xfs_inode xfs_bmap_util
xfs_bmap_item xfs_rmap_item xfs_refcount_item xfs_attr_item xfs_attr_inactive
xfs_log_recover"
rm -rf "$OUT"; mkdir -p "$OUT/logs" "$OUT/gen" "$OUT/pre/linux"
EXTRA=()
if [ $pre = 1 ]; then
  printf '#define __releases(x)\n#define __acquires(x)\n#define __must_hold(x)\n#define ____cacheline_aligned_in_smp\n' > "$OUT/pre/pre.h"
  : > "$OUT/pre/linux/iversion.h"; : > "$OUT/pre/xfs_qm.h"
  EXTRA=(-include "$OUT/pre/pre.h" -I"$OUT/pre")
fi
FL=(-std=gnu11 -fsyntax-only -fmax-errors=0 -Wno-sign-compare -Wno-unused-parameter
    -Wno-missing-field-initializers -Wno-type-limits -Wno-implicit-fallthrough
    -Wno-empty-body -Wno-override-init -Wno-parentheses -Wno-address-of-packed-member
    -Ibackends/xfs/shim "${EXTRA[@]}" -Iinclude -I$K/libxfs -I$K)
for f in $LIBXFS; do gcc "${FL[@]}" "$K/libxfs/$f.c" > "$OUT/logs/$f.log" 2>&1 & done
for f in $TOP; do cp "$K/$f.c" "$OUT/gen/"; gcc "${FL[@]}" "$OUT/gen/$f.c" > "$OUT/logs/$f.log" 2>&1 & done
wait
for f in $LIBXFS $TOP; do printf "%-24s %4s errors\n" "$f" "$(grep -c ' error: ' "$OUT/logs/$f.log" || true)"; done >&2
python3 -I tools/xfs_shim_needs.py "$OUT/logs" $pre
