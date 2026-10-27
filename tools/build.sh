#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-linux}" in
  linux) cargo build --workspace ;;
  windows) cargo build --workspace --target x86_64-pc-windows-gnu ;;
  *) echo "usage: $0 [linux|windows]" >&2; exit 2 ;;
esac
