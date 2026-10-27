#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cargo fmt --check
cargo clippy --workspace -- -D warnings
cargo test --workspace
if [ -f deny.toml ]; then
  cargo deny check
fi
