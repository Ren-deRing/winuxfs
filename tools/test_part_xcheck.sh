#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Cross-checks `winuxfs list-partitions` against sgdisk on GPT images that
# sgdisk itself creates: every partition's number, start and length (bytes)
# must match `sgdisk -p`.
#
# Usage: tools/test_part_xcheck.sh [path/to/winuxfs]
#        (default: $WINUXFS_BIN, else target/debug/winuxfs)
# Exit: 0 match, 1 mismatch or error, 77 skipped (sgdisk not installed).
set -euo pipefail

bin=${1:-${WINUXFS_BIN:-$(dirname "$0")/../target/debug/winuxfs}}

if ! command -v sgdisk >/dev/null 2>&1; then
  echo "sgdisk not found: install the gdisk package (for example 'dnf install gdisk' or 'apt install gdisk') to run this check" >&2
  exit 77
fi
if [ ! -x "$bin" ]; then
  echo "winuxfs binary not found at $bin: build it with 'cargo build -p winuxfs-cli' or pass its path" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Rows "number start_bytes length_bytes", from sgdisk.
expected() {
  sgdisk -p "$1" | awk '
    /^Sector size \(logical\)/ { ss = $4 }
    /^ *[0-9]+ +[0-9]+ +[0-9]+ / { print $1, $2 * ss, ($3 - $2 + 1) * ss }'
}

# Same rows from winuxfs (rows whose first column is a number).
actual() {
  "$bin" list-partitions "$1" | awk '$1 ~ /^[0-9]+$/ { print $1, $2, $3 }'
}

check() {
  local name=$1 img=$2
  expected "$img" >"$tmp/$name.exp"
  actual "$img" >"$tmp/$name.act"
  if [ ! -s "$tmp/$name.exp" ]; then
    echo "$name: sgdisk listed no partitions" >&2
    return 1
  fi
  if ! diff -u "$tmp/$name.exp" "$tmp/$name.act"; then
    echo "$name: winuxfs list-partitions disagrees with sgdisk -p" >&2
    return 1
  fi
  echo "$name: $(wc -l <"$tmp/$name.exp") partitions match"
}

# 1: three partitions, the last one filling the disk.
truncate -s 64M "$tmp/three.img"
sgdisk -o -n 1:2048:+8M -t 1:8300 -c 1:alpha -n 2:0:+16M -t 2:0700 \
  -n 3:0:0 -t 3:8300 "$tmp/three.img" >/dev/null
check three "$tmp/three.img"

# 2: a gap and odd alignment, partitions created out of order.
truncate -s 32M "$tmp/gaps.img"
sgdisk -o -a 1 -n 4:40000:+1M -n 2:100:+300K -n 7:34:99 "$tmp/gaps.img" >/dev/null
check gaps "$tmp/gaps.img"

echo "ok"
