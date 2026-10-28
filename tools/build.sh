#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds the Rust workspace and the C backend plugins (into target/plugins).
set -euo pipefail
cd "$(dirname "$0")/.."
PLUGINS=(memfs)
case "${1:-linux}" in
  linux)
    cargo build --workspace
    for p in "${PLUGINS[@]}"; do make -C "backends/$p" CC="${CC:-cc}" EXT=so; done
    ;;
  windows)
    cargo build --workspace --target x86_64-pc-windows-gnu
    for p in "${PLUGINS[@]}"; do
      make -C "backends/$p" CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}" EXT=dll
    done
    ;;
  *) echo "usage: $0 [linux|windows]" >&2; exit 2 ;;
esac
