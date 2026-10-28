#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cargo fmt --check
cargo clippy --workspace -- -D warnings
cargo test --workspace
python3 -m unittest tools/test_license_scan.py
python3 -m unittest tools/test_mkdisk.py
# Both need the winuxfs binary, which cargo test above builds.
export WINUXFS_BIN="${CARGO_TARGET_DIR:-target}/debug/winuxfs"
python3 -m unittest tools/test_cli_errors.py
skipped=()
rc=0
tools/test_part_xcheck.sh "$WINUXFS_BIN" || rc=$?
case $rc in
  0) ;;
  77) skipped+=("tools/test_part_xcheck.sh: sgdisk missing (install the gdisk package)") ;;
  *) exit "$rc" ;;
esac
if [ -f deny.toml ]; then
  cargo deny check
fi
if [ ${#skipped[@]} -gt 0 ]; then
  for s in "${skipped[@]}"; do echo "SKIPPED: $s"; done
fi

