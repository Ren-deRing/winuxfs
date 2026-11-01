#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
# Usage: tools/check.sh [--vm] [--fuzz]
#   (no flag)  fast host checks (including test-fuzz: corpus + clean walk, seconds).
#   --fuzz     the same, then `make fuzz-sample`: the seed-fixed 1% sample of all
#              sector mutations in both CRC modes (about a minute; the full set is
#              `make -C backends/xfs fuzz-full`, for nightly runs).
#   --vm       the same, then the full Milestone 1 and XFS (M2) acceptance: Windows
#              cross-build, VM restored to snapshot `clean`, every Windows
#              test executable (integration tests e2e_*, ffi_* and the
#              lib/bin unit tests of winuxfs-win and winuxfs-cli) run in the VM.
#              WINUXFS_VMCTL overrides the vmctl.py to use (a worktree has no
#              vm/ state; point it at the main checkout's tools/vmctl.py).
vm=0
fuzz=0
for a in "$@"; do
  case "$a" in
    --vm) vm=1 ;;
    --fuzz) fuzz=1 ;;
    *) echo "usage: $0 [--vm] [--fuzz]" >&2; exit 2 ;;
  esac
done
cargo fmt --check
cargo clippy --workspace -- -D warnings
cargo test --workspace
tools/vendor-xfs.sh --verify
(cd third_party/linux-lib && sha256sum -c --quiet SOURCE.sha256)
if command -v gcc >/dev/null; then
  make -s -C backends/xfs test-shim
  make -s -C backends/xfs test-rbtree
  make -s -C backends/xfs test-buf
  make -s -C backends/xfs tsan-buf
else
  echo "SKIPPED: backends/xfs shim and buffer cache tests (gcc not installed)"
fi
if command -v gcc >/dev/null; then
  make -s -C backends/xfs test-logcheck
else
  echo "SKIPPED: backends/xfs logcheck test (gcc not installed)"
fi
if command -v gcc >/dev/null && command -v mkfs.xfs >/dev/null && command -v xfs_db >/dev/null; then
  make -s -C backends/xfs test-mount
  make -s -C backends/xfs test-mount-san
  make -s -C backends/xfs test-icache
  make -s -C backends/xfs test-icache-san
  make -s -C backends/xfs test-statfs
  make -s -C backends/xfs test-dir
  make -s -C backends/xfs test-dir-san
  make -s -C backends/xfs test-read
  make -s -C backends/xfs test-read-san
  make -s -C backends/xfs test-concurrent-san
  make -s -C backends/xfs test-concurrent-tsan
  make -s -C backends/xfs test-fuzz
  make -s -C backends/xfs test-rw
  make -s -C backends/xfs test-rw-san
  make -s -C backends/xfs test-rw-tsan
  # M3a write suites: native, then the same on rmapbt+reflink images (M3d), plus TSan on the stress
  make -s -C backends/xfs test-wops
  make -s -C backends/xfs test-wdata
  make -s -C backends/xfs test-wlink
  make -s -C backends/xfs test-recover
  WCRASH_POINTS=400 make -s -C backends/xfs test-crash
  make -s -C backends/xfs test-wstress
  WCRASH_POINTS=400 make -s -C backends/xfs test-wrmap
  make -s -C backends/xfs test-wstress-tsan
  # fuzzing after writes: mutated images mounted read-write and written to (ASan+UBSan, CRC on and off)
  make -s -C backends/xfs test-wfuzz-all
  if [ "$fuzz" = 1 ]; then make -s -C backends/xfs fuzz-sample; fi
else
  echo "SKIPPED: backends/xfs mount tests (gcc, mkfs.xfs and xfs_db needed; install xfsprogs)"
fi
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  make -s -C backends/xfs CROSS=x86_64-w64-mingw32- test-shim-build
else
  echo "SKIPPED: backends/xfs mingw shim build (x86_64-w64-mingw32-gcc not installed)"
fi
if command -v gcc >/dev/null; then
  make -s -C backends/xfs test-link
else
  echo "SKIPPED: backends/xfs libxfs link test (gcc not installed)"
fi
python3 -I tools/license-scan.py --check-allowed third_party/linux-xfs
python3 -I tools/license-scan.py --check-allowed third_party/linux-lib
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

# ---- XFS stage (M2 design 10.7): GPT disk of fixture images, the xfs plugin
# and tools/xfs-vm-acceptance.ps1 in the VM, host-side diff of its manifests.
echo "== VM: XFS acceptance"
XFS_FIX="${XFS_FIXTURES:-tests/xfs/images}"
if [ ! -f "$XFS_FIX/hole-unwritten.img" ]; then tools/gen-fixtures.sh --out "$XFS_FIX"; fi
XFS_DIR=target/xfs-vm
rm -rf "$XFS_DIR/vmout"
python3 -I tools/xfs_vm_acceptance.py prepare "$XFS_DIR" --fixtures "$XFS_FIX"
XFS_STAGE=target/vm-stage-xfs
rm -rf "$XFS_STAGE"
mkdir -p "$XFS_STAGE"
cp "$WIN/winuxfs.exe" target/plugins-win/winuxfs_backend_xfs.dll tools/xfs-vm-acceptance.ps1 "$XFS_STAGE/"
vmctl restore clean
vmctl attach-disk "$XFS_DIR/disk.img"
vmctl push "$XFS_STAGE" t15 >/dev/null
vmctl run -- 'if exist C:\t15 rmdir /s /q C:\t15'
vmctl run -- 'xcopy /y /e /i /q \\host.lan\Data\t15 C:\t15'
xfs_rc=0
vmctl run -- 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\t15\xfs-vm-acceptance.ps1 -Dir C:\t15 -Out C:\t15\out' || xfs_rc=$?
vmctl run -- 'if exist \\host.lan\Data\t15out rmdir /s /q \\host.lan\Data\t15out'
vmctl run -- 'xcopy /y /e /i /q C:\t15\out \\host.lan\Data\t15out'
XFS_OUT="$(dirname "$VMCTL")/../vm/shared/t15out"
if [ "$xfs_rc" != 0 ]; then echo "XFS acceptance script failed ($xfs_rc)" >&2; exit 1; fi
vmctl down
python3 -I tools/xfs_vm_acceptance.py verify "$XFS_DIR" "$XFS_OUT" --data-disk "$(dirname "$VMCTL")/../vm/storage2/data2.img"
echo "VM acceptance OK"
