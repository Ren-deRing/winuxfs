#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Usage: tools/apply-xfs-patches.sh OUTDIR
# Copies third_party/linux-xfs/ to OUTDIR and applies third_party/linux-xfs-patches/*.patch
# in lexical order. The vendored tree is never modified.
# Patch paths are the file's base name (`--- a/xfs_dir2_leaf.c`), the form the
# backends/xfs makefiles use when they patch single files; each patch is
# applied (-p1) inside the directory that holds the file (tree root or libxfs/).
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# = 1 ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
out=$1
rm -rf "$out"
mkdir -p "$out"
cp -a third_party/linux-xfs/. "$out/"
shopt -s nullglob
for p in third_party/linux-xfs-patches/*.patch; do
  f=$(sed -n 's/^--- a\/\([^[:space:]]*\).*/\1/p' "$p" | head -1)
  [ -n "$f" ] || { echo "$p: no '--- a/' line" >&2; exit 1; }
  dir=
  for c in "$out" "$out/libxfs"; do
    [ -f "$c/$f" ] && { dir=$c; break; }
  done
  [ -n "$dir" ] || { echo "$p: $f not found in the vendored tree" >&2; exit 1; }
  echo "applying $p to $dir/$f"
  chmod u+w "$dir/$f"
  patch -s -d "$dir" -p1 --no-backup-if-mismatch < "$p"
done
