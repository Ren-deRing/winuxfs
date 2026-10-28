#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Vendor the Linux fs/xfs read path into third_party/linux-xfs/ (byte for byte).
#
# Usage:
#   tools/vendor-xfs.sh --src DIR   copy from DIR (a checkout of fs/xfs at the
#                                   pinned commit); idempotent
#   tools/vendor-xfs.sh --verify    re-hash third_party/linux-xfs against SOURCE;
#                                   fail on any drift, missing or extra file
#
# DIR is the fs/xfs directory itself, e.g. <linux>/fs/xfs. No network access.
# To get it on a host with network: git clone --filter=blob:none --no-checkout
# https://github.com/torvalds/linux; git -C linux sparse-checkout set fs/xfs;
# git -C linux checkout d135ab453a7400a34930bdda1ef4bdc8acfac80e
# Env VENDOR_DIR overrides the destination (tests use it).
set -euo pipefail
cd "$(dirname "$0")/.."

COMMIT=d135ab453a7400a34930bdda1ef4bdc8acfac80e
DEST=${VENDOR_DIR:-third_party/linux-xfs}

# M2 design 5.1: the 29 libxfs read-path .c files (all libxfs/*.h are copied too).
LIBXFS_C="xfs_sb xfs_ag xfs_group xfs_types xfs_bit xfs_ialloc xfs_ialloc_btree
xfs_alloc xfs_alloc_btree xfs_btree xfs_bmap xfs_bmap_btree xfs_iext_tree
xfs_inode_buf xfs_inode_fork xfs_inode_util xfs_da_btree xfs_dir2 xfs_dir2_block
xfs_dir2_data xfs_dir2_leaf xfs_dir2_node xfs_dir2_sf xfs_symlink_remote
xfs_attr_leaf xfs_rtgroup xfs_metafile xfs_rmap_btree xfs_refcount_btree"
# M2 design 5.3: top-level files that are ported with edits (E).
TOP_FILES="xfs_dir2_readdir.c xfs_mount.c xfs_icache.c xfs_symlink.c
xfs_log_recover.c xfs_log_priv.h"
# Headers #included by the files above that live in fs/xfs (not libxfs/). They
# are kept as reference; backends/xfs/shim/ provides the headers actually used.
EXTRA_HDRS="xfs_bmap_util.h xfs_buf_mem.h xfs_dquot.h xfs_error.h
xfs_extent_busy.h xfs_filestream.h xfs_fsops.h xfs_healthmon.h xfs_icache.h
xfs_inode.h xfs_iomap.h xfs_log.h xfs_mount.h xfs_platform.h xfs_quota.h
xfs_reflink.h xfs_rtalloc.h xfs_symlink.h xfs_sysfs.h xfs_trace.h
xfs_trans_priv.h xfs_zone_alloc.h scrub/stats.h"

die() { echo "vendor-xfs: $*" >&2; exit 1; }

verify() {
  [ -d "$DEST" ] || die "$DEST missing"
  [ -f "$DEST/SOURCE" ] || die "$DEST/SOURCE missing"
  local rc=0 recorded actual
  recorded=$(grep -Ev '^(#|commit: |$)' "$DEST/SOURCE" | sort -k2)
  actual=$(cd "$DEST" && find . -type f ! -name SOURCE | sed 's|^\./||' | LC_ALL=C sort \
    | while IFS= read -r f; do sha256sum -- "$f"; done | sort -k2)
  if [ "$recorded" != "$actual" ]; then
    echo "vendor-xfs: drift between SOURCE and $DEST:" >&2
    diff <(echo "$recorded") <(echo "$actual") >&2 || true
    rc=1
  fi
  grep -qx "commit: $COMMIT" "$DEST/SOURCE" || { echo "vendor-xfs: SOURCE commit mismatch" >&2; rc=1; }
  [ $rc = 0 ] && echo "vendor-xfs: verify OK ($(echo "$recorded" | wc -l) files)"
  return $rc
}

vendor() {
  local src=$1
  [ -n "$src" ] && [ -d "$src" ] || die "--src DIR missing or not a directory: ${src:-<none>}"
  [ -d "$src/libxfs" ] || die "$src has no libxfs/ (pass the fs/xfs directory)"
  if git -C "$src" rev-parse HEAD >/dev/null 2>&1; then
    local head
    head=$(git -C "$src" rev-parse HEAD)
    [ "$head" = "$COMMIT" ] || die "source is at $head, expected $COMMIT"
  else
    echo "vendor-xfs: warning: cannot confirm source commit (not a git checkout)" >&2
  fi
  local list core extra f
  core=$( { for b in $LIBXFS_C; do echo "libxfs/$b.c"; done
            (cd "$src" && ls libxfs/*.h)
            for t in $TOP_FILES; do echo "$t"; done; } | LC_ALL=C sort)
  extra=$(for h in $EXTRA_HDRS; do echo "$h"; done | LC_ALL=C sort)
  for f in $core $extra; do [ -f "$src/$f" ] || die "not found in source: $f"; done
  # Rebuild from scratch so removed entries cannot linger.
  rm -rf "$DEST"
  for f in $core $extra; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp -- "$src/$f" "$DEST/$f"
  done
  {
    echo "# Vendored from the Linux kernel, fs/xfs (GPL-2.0 family; see docs/licensing.md)."
    echo "# Files are byte-for-byte copies. Modify only through third_party/linux-xfs-patches/."
    echo "# Written by tools/vendor-xfs.sh; check with tools/vendor-xfs.sh --verify."
    echo "commit: $COMMIT"
    echo "# --- core: M2 design 5.1 libxfs .c files, all libxfs/*.h, 5.3 top-level files"
    for f in $core; do (cd "$DEST" && sha256sum -- "$f"); done
    echo "# --- extra: fs/xfs headers #included by the files above (include closure, reference only;"
    echo "# --- the shim in backends/xfs/shim/ supplies the headers actually compiled against)"
    for f in $extra; do (cd "$DEST" && sha256sum -- "$f"); done
  } > "$DEST/SOURCE"
  echo "vendor-xfs: vendored $(( $(echo "$core" | wc -l) + $(echo "$extra" | wc -l) )) files into $DEST"
  verify
}

case "${1:-}" in
  --verify) [ $# = 1 ] || die "usage: $0 [--src DIR | --verify]"; verify ;;
  --src) [ $# = 2 ] || die "usage: $0 [--src DIR | --verify]"; vendor "$2" ;;
  *) die "usage: $0 [--src DIR | --verify]" ;;
esac
