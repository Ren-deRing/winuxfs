#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
# Usage: tools/check.sh [--vm]
#   (no flag)  fast host checks.
#   --vm       the same, then the full Milestone 1 acceptance: Windows
#              cross-build, VM restored to snapshot `clean`, every Windows
#              test executable (integration tests e2e_*, ffi_* and the
#              lib/bin unit tests of winuxfs-win and winuxfs-cli) run in the VM.
#              WINUXFS_VMCTL overrides the vmctl.py to use (a worktree has no
#              vm/ state; point it at the main checkout's tools/vmctl.py).
vm=0
case "${1:-}" in
  "") ;;
  --vm) vm=1 ;;
  *) echo "usage: $0 [--vm]" >&2; exit 2 ;;
esac
cargo fmt --check
cargo clippy --workspace -- -D warnings
cargo test --workspace
python3 -m unittest tools/test_license_scan.py
python3 -m unittest tools/test_mkdisk.py
# Windows-only code is never compiled by the host run above: check and lint
# it for the Windows target too (skipped with a note if the target or the
# mingw linker is missing).
WINTARGET=x86_64-pc-windows-gnu
if rustup target list --installed 2>/dev/null | grep -qx "$WINTARGET" \
    && command -v x86_64-w64-mingw32-gcc >/dev/null; then
  for p in winuxfs-core winuxfs-conformance winuxfs-win winuxfs-cli; do
    cargo check -p "$p" --target "$WINTARGET" --all-targets
    cargo clippy -p "$p" --target "$WINTARGET" --all-targets -- -D warnings
  done
else
  echo "NOTE: Windows target checks skipped ($WINTARGET or x86_64-w64-mingw32-gcc not installed)"
fi
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
rc=0
tools/gen-fixtures.sh --self-test || rc=$?
case $rc in
  0) ;;
  77) skipped+=("tools/gen-fixtures.sh --self-test: xfsprogs missing (install xfsprogs)") ;;
  *) exit "$rc" ;;
esac
python3 -m unittest tools.test_xfs_manifest
if [ -f deny.toml ]; then
  cargo deny check
fi
if [ ${#skipped[@]} -gt 0 ]; then
  for s in "${skipped[@]}"; do echo "SKIPPED: $s"; done
fi


[ "$vm" = 1 ] || exit 0

echo "== Windows cross-build"
tools/build.sh windows
WIN=target/x86_64-pc-windows-gnu/debug
make -C backends/testplug CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}" EXT=dll OUT="$PWD/target/test-plugins-win"
# Test executables (integration tests and the lib/bin unit tests) of
# winuxfs-win and winuxfs-cli, located via cargo's JSON.
STAGE=target/vm-stage
rm -rf "$STAGE"
mkdir -p "$STAGE/testplug"
cargo test -p winuxfs-win -p winuxfs-cli --target x86_64-pc-windows-gnu --no-run \
  --message-format=json > target/vm-tests.json
python3 -I - target/vm-tests.json "$STAGE" <<'PY'
import json, shutil, sys
n = 0
for line in open(sys.argv[1]):
    try:
        m = json.loads(line)
    except ValueError:
        continue
    if m.get("reason") == "compiler-artifact" and m.get("executable") \
            and m["profile"]["test"]:
        # winuxfs-cli's integration tests are host tests (they build a C
        # plugin with make): only its unit tests run in the VM.
        if "winuxfs-cli" in m["package_id"] and m["target"]["kind"] == ["test"]:
            continue
        # The file name carries cargo's hash, so the unit-test executable of
        # the winuxfs bin cannot collide with winuxfs.exe.
        dst = m["executable"].rsplit("/", 1)[-1]
        shutil.copy2(m["executable"], f"{sys.argv[2]}/{dst}")
        n += 1
if n == 0:
    sys.exit("no test executables found")
PY
cp "$WIN/winuxfs.exe" target/plugins/winuxfs_backend_memfs.dll "$STAGE/"
cp target/test-plugins-win/winuxfs_backend_testplug.dll "$STAGE/testplug/"

VMCTL="${WINUXFS_VMCTL:-tools/vmctl.py}"
vmctl() { python3 "$VMCTL" "$@"; }
started=0
if ! vmctl status >/dev/null 2>&1; then started=1; fi
cleanup() {
  if [ "$started" = 1 ]; then vmctl down || true; fi
}
trap cleanup EXIT

echo "== VM: restore clean, attach pattern disk"
vmctl restore clean
rm -f target/pattern.img
python3 -I tools/mkdisk.py target/pattern.img
vmctl attach-disk target/pattern.img
vmctl push "$STAGE" t16 >/dev/null
vmctl run -- 'if exist C:\t16 rmdir /s /q C:\t16'
vmctl run -- 'xcopy /y /e /i /q \\host.lan\Data\t16 C:\t16'

echo "== VM: running Windows tests"
failed=()
for exe in "$STAGE"/*.exe; do
  name=$(basename "$exe")
  [ "$name" = winuxfs.exe ] && continue
  echo "-- $name"
  if ! vmctl run -- "C:\\t16\\$name"; then failed+=("$name"); fi
done
if [ ${#failed[@]} -gt 0 ]; then
  echo "FAILED in VM: ${failed[*]}" >&2
  exit 1
fi
echo "VM acceptance OK"
