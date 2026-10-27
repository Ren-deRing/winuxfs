#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cargo fmt --check
cargo clippy --workspace -- -D warnings
cargo test --workspace
python3 -m unittest tools/test_license_scan.py
python3 -m unittest tools/test_mkdisk.py
if [ -f deny.toml ]; then
  cargo deny check
fi
