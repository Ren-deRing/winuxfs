#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Task 4 link tests, run by `make test-link` (cwd = backends/xfs).
#   all_29_files_compile_without_warnings
#   no_unresolved_symbols_native_and_mingw
#   plugin_loads_and_reports_abi_version
#   write_path_symbols_do_not_abort
# Env: CC (default gcc), CROSS (set => only the cross build is exercised).
# Missing compilers are reported as SKIPPED, never as success.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd ../.. && pwd)
fails=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1: $(echo "$2" | cut -c1-400)"; fails=$((fails + 1)); }
M="make -s --no-print-directory"
MINGW=x86_64-w64-mingw32-
have_native=0; have_mingw=0
command -v gcc >/dev/null && have_native=1
command -v ${MINGW}gcc >/dev/null && have_mingw=1

# --- 1. all 29 files compile (-Wall -Wextra -Werror + the kernel-code
# relaxations of the Makefile). Objects are NOT cleaned first: -Werror means an
# object that exists was built without warnings, and make rebuilds on change. ---
t=all_29_files_compile_without_warnings
for tc in native mingw; do
  if [ $tc = native ]; then [ $have_native = 1 ] || { echo "SKIPPED $t[native]: gcc not installed"; continue; }; cross=
  else [ $have_mingw = 1 ] || { echo "SKIPPED $t[mingw]: ${MINGW}gcc not installed"; continue; }; cross=$MINGW; fi
  out=$($M CROSS=$cross OUT=$ROOT/target/xfs-link-$tc libxfs-objs 2>&1); rc=$?
  n=$(find "$ROOT/target/xfs-link-$tc/libxfs" -name '*.o' 2>/dev/null | wc -l)
  if [ $rc -ne 0 ] || [ "$n" -ne 29 ] || echo "$out" | grep -q 'warning'; then
    fail "$t[$tc]" "rc=$rc objects=$n $(echo "$out" | grep -m3 -E 'error|warning')"
  else pass "$t[$tc]: 29 objects"; fi
done

# --- 2. no unresolved symbols; plugin imports only libc / OS ---
t=no_unresolved_symbols_native_and_mingw
check_so() { # $1 = shared object: every undefined non-weak symbol must be a versioned glibc one
  bad=$(nm -D -u "$1" | awk '$1!="w" && $1!="v" {print $2}' | grep -v '@GLIBC' | tr '\n' ' ')
  [ -z "$bad" ] || echo "non-libc imports: $bad"
}
check_dll() { # $1 = dll: imports only from the OS/CRT DLLs
  bad=$(${MINGW}objdump -p "$1" | awk '/DLL Name:/ {print $3}' | grep -vi -E '^(KERNEL32|msvcrt|api-ms-win-crt-[a-z0-9-]+|ntdll|ADVAPI32)\.dll$' | tr '\n' ' ')
  [ -z "$bad" ] || echo "unexpected DLL imports: $bad"
}
if [ $have_native = 1 ]; then
  err=$($M OUT=$ROOT/target/xfs-link-native PLUGIN_DIR=$ROOT/target/xfs-link-native/plugin plugin link-full 2>&1) || err="make failed: $(echo "$err" | grep -m5 -E 'undefined|error')"
  if [ -z "$err" ]; then
    for so in $ROOT/target/xfs-link-native/plugin/winuxfs_backend_xfs.so $ROOT/target/xfs-link-native/full_check.so; do
      err="$err$(check_so "$so")"
    done
    exports=$(nm -D --defined-only $ROOT/target/xfs-link-native/plugin/winuxfs_backend_xfs.so | awk '{print $3}' | tr '\n' ' ')
    [ "$exports" = "wfs_backend_entry " ] || err="$err so exports: $exports"
  fi
  [ -z "$err" ] && pass "$t[native]: plugin + all-sections link, libc-only imports" || fail "$t[native]" "$err"
else echo "SKIPPED $t[native]: gcc not installed"; fi
if [ $have_mingw = 1 ]; then
  err=$($M CROSS=$MINGW OUT=$ROOT/target/xfs-link-mingw PLUGIN_DIR=$ROOT/target/xfs-link-mingw/plugin plugin link-full 2>&1) || err="make failed: $(echo "$err" | grep -m5 -E 'undefined|error')"
  if [ -z "$err" ]; then
    for d in $ROOT/target/xfs-link-mingw/plugin/winuxfs_backend_xfs.dll $ROOT/target/xfs-link-mingw/full_check.dll; do
      err="$err$(check_dll "$d")"
    done
    exports=$(${MINGW}objdump -p $ROOT/target/xfs-link-mingw/plugin/winuxfs_backend_xfs.dll | sed -n '/^\[Ordinal\/Name Pointer\] Table/,$p' | grep -E '^[[:space:]]*\[' | awk '$NF !~ /^[0-9]+$/ {print $NF}' | tr '\n' ' ')
    [ "$exports" = "wfs_backend_entry " ] || err="$err dll exports: $exports"
  fi
  [ -z "$err" ] && pass "$t[mingw]: plugin + all-sections link, KERNEL32/msvcrt-only imports, only wfs_backend_entry exported" || fail "$t[mingw]" "$err"
else echo "SKIPPED $t[mingw]: ${MINGW}gcc not installed"; fi

# --- 3. the host lists the plugin as backend `xfs` (the loader rejects a bad
# abi_version/size, so a listing line proves the ABI check passed) ---
t=plugin_loads_and_reports_abi_version
so=$ROOT/target/xfs-link-native/plugin/winuxfs_backend_xfs.so
if [ $have_native = 0 ]; then echo "SKIPPED $t: gcc not installed"
elif [ ! -f "$so" ]; then fail $t "plugin not built"
else
  bin=${WINUXFS_BIN:-$ROOT/target/debug/winuxfs}
  if [ ! -x "$bin" ]; then
    (cd "$ROOT" && cargo build -q -p winuxfs-cli) >/dev/null 2>&1
    bin=$ROOT/target/debug/winuxfs
  fi
  if [ ! -x "$bin" ]; then echo "SKIPPED $t: winuxfs binary not available (cargo build -p winuxfs-cli)"
  else
    line=$(env -u WINUXFS_PLUGIN_PATH "$bin" list-backends --plugin-dir "$ROOT/target/xfs-link-native/plugin" 2>&1)
    case "$line" in
      xfs\ *caps=readonly,hardlink,symlink*winuxfs_backend_xfs.so*) pass "$t: $(echo "$line" | tr -s ' ')" ;;
      *) fail $t "unexpected list-backends output: $line" ;;
    esac
  fi
fi

# --- 4. write-path symbols resolve to stubs and the stubs do not abort ---
t=write_path_symbols_do_not_abort
if [ $have_native = 0 ]; then echo "SKIPPED $t: gcc not installed"
else
  obj=$ROOT/target/xfs-link-native/be/stubs_ro.o
  miss=
  for s in xfs_trans_alloc xfs_trans_commit xfs_trans_roll xfs_defer_finish xfs_trans_log_inode \
           xfs_trans_log_buf xfs_trans_ijoin xfs_rmap_alloc xfs_rmap_free xfs_extent_busy_insert \
           xfs_buf_delwri_submit xfs_dec_freecounter xfs_add_freecounter xfs_icreate_log \
           xfs_trans_mod_sb xfs_parent_addname xfs_refcount_alloc_cow_extent; do
    nm "$obj" | grep -q " T $s\$" || miss="$miss $s"
    # and the full link takes exactly that definition (no second one elsewhere)
    n=$(nm "$ROOT/target/xfs-link-native"/libxfs/*.o "$ROOT/target/xfs-link-native"/shim/*.o | grep -c " [TtWw] $s\$")
    [ "$n" = 0 ] || miss="$miss $s(also defined in libxfs/shim)"
  done
  r=$($M OUT=$ROOT/target/xfs-link-native test-stubs 2>&1) || miss="$miss test_stubs: $r"
  [ -z "$miss" ] && pass "$t: $(echo "$r" | tail -1)" || fail $t "$miss"
fi

[ $fails -eq 0 ] || { echo "test_link: $fails FAILED"; exit 1; }
echo "test_link: OK"
