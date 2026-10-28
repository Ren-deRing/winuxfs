#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Usage: tools/apply-xfs-patches.sh OUTDIR
# Copies third_party/linux-xfs/ to OUTDIR and applies third_party/linux-xfs-patches/*.patch
# in lexical order (-p1). The vendored tree is never modified.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# = 1 ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
out=$1
rm -rf "$out"
mkdir -p "$out"
cp -a third_party/linux-xfs/. "$out/"
shopt -s nullglob
for p in third_party/linux-xfs-patches/*.patch; do
  echo "applying $p"
  patch -d "$out" -p1 --no-backup-if-mismatch < "$p"
done
