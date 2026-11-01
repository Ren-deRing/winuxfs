# Licensing

winuxfs is licensed **GPL-2.0-only** (see `LICENSE`). New files carry
`SPDX-License-Identifier: GPL-2.0-only`. GPL-3.0 code is not mixed into the
sources or the distributed binaries.

> This is an engineering reading of the license texts, not legal advice.

## 1. Linux `fs/xfs` sources

The XFS backend compiles files from the Linux kernel's `fs/xfs`. A scan of
kernel commit `d135ab453a7400a34930bdda1ef4bdc8acfac80e` (2026-10-26) found:

| License | Files |
|---|---:|
| `GPL-2.0-only` (written `GPL-2.0` in the files) | 187 |
| `GPL-2.0-or-later` | 146 |
| `GPL-2.0+` (the deprecated spelling of `GPL-2.0-or-later`) | 22 |
| `LGPL-2.1` (`libxfs/xfs_fs.h`) | 1 |
| no identifier | 0 |
| **total** | **356** |

Only `Makefile` and `Kconfig` are not `*.c`/`*.h`; they are build files and are
not ported.

Why this is compatible with GPL-2.0-only:

- `GPL-2.0-only` files keep the same terms.
- `GPL-2.0-or-later` and `GPL-2.0+` files let the recipient choose GPLv2 or a
  later version, so choosing GPLv2 and combining them with GPL-2.0-only code
  breaks no file's terms.
- `LGPL-2.1` section 3 allows applying the ordinary GPL (v2 or later) to the
  code instead, so `xfs_fs.h` can be used under GPLv2.
- The combined work cannot be relicensed as GPLv3, because 187 files are
  GPL-2.0-only. That is why GPL-3.0-only code cannot be mixed in.

Ported files keep their original SPDX lines and copyright notices.

### Vendored sources

- `third_party/linux-xfs/` holds the kernel files byte for byte. Per-file
  SHA-256 hashes and the commit are in `third_party/linux-xfs/SOURCE`;
  `tools/vendor-xfs.sh --verify` (run by `tools/check.sh`) fails on any
  difference, missing file or extra file. The tree is never edited: changes are
  patches in `third_party/linux-xfs-patches/`, applied by
  `tools/apply-xfs-patches.sh` onto a copy in the build directory.
- `third_party/linux-lib/` holds the kernel's `lib/rbtree.c` and the rbtree
  headers (all `GPL-2.0-or-later`), checked with `SOURCE.sha256`. Its patches
  live in `third_party/linux-lib-patches/`.
- `python3 -I tools/license-scan.py --check-allowed third_party/linux-xfs`
  (run by `tools/check.sh`, tested by `tools/test_license_scan.py`) compares
  the SPDX identifier of every `.c` and `.h` file with an allowed set containing
  only what is actually vendored: `GPL-2.0-only`, `GPL-2.0-or-later`,
  `GPL-2.0+` and `LGPL-2.1`. `GPL-3.0*`, `AGPL*`, `LGPL-3.0*`, unknown
  identifiers and files without an SPDX line fail the scan.

## 2. WinFsp

winuxfs needs WinFsp (<https://github.com/winfsp/winfsp>) installed on the
Windows machine. WinFsp is GPLv3 with a FLOSS exception (`License.txt`, master
commit `4425d4807aea4626affb151e38c1cc774ccd5e75`):

> As a special exception to GPLv3, Bill Zissimopoulos grants additional
> permissions to Free/Libre and Open Source Software ("FLOSS") without
> requiring that such software is covered by the GPLv3.
>
> 1. Permission to link with a platform specific version of the WinFsp DLL.
> 2. Permission to distribute unmodified binary releases of the WinFsp
>    installer.
>
> These permissions (and no other) are granted provided that the software:
> 1. Is distributed under a license that satisfies the Free Software
>    Definition or the Open Source Definition.
> 2. Includes the copyright notice "WinFsp - Windows File System Proxy,
>    Copyright (C) Bill Zissimopoulos" and a link to the WinFsp repository in
>    its user-interface and any user-facing documentation.
> 3. Is not linked or distributed with proprietary (non-FLOSS) software.

GPL-2.0-only satisfies the first condition: it is a free software license by
the FSF's definition and approved by the OSI. The exception names no list of
licenses, only these definitions.

The third condition does not exclude operating system components such as
`kernel32.dll`, which every Windows program links; the exception's purpose is
to allow FLOSS file systems on Windows, and that is the reading used here.

### Decision

winuxfs uses WinFsp this way:

- `winfsp-x64.dll` is loaded at run time with `LoadLibrary`/`GetProcAddress`.
  The import library is never linked.
- The needed functions and structures are declared by hand in
  `crates/winuxfs-win/src/winfsp_ffi.rs`. `winfsp.h` is not included and no
  bindings are generated from it. The `winfsp` and `winfsp-sys` crates (GPLv3)
  are not used.
- The WinFsp DLL and installer are not bundled or redistributed. Users install
  WinFsp from the official installer.
- The CLI (`--version`, `--help`) and the README name WinFsp:
  "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos"
  and `https://github.com/winfsp/winfsp`.
- winuxfs is not linked or distributed with proprietary software, apart from
  operating system components.

winuxfs distributes only GPLv2 sources and binaries built from them, and
WinFsp is combined with them only on the user's machine at run time. GPLv2 does
not restrict running a program. The WinFsp exception removes GPLv3's
requirements for the winuxfs side, so no conflict is expected.

A stricter reading of GPLv2 section 2 is conceivable for a work that is
distributed together with a GPLv3 DLL, because that DLL cannot be licensed
under GPLv2. It does not arise while WinFsp is not distributed with winuxfs;
if that ever changes, take legal advice first.

### What was read

The declarations in `winfsp_ffi.rs` follow the public WinFsp v2.1 header
(structure sizes: volume parameters 504 bytes, volume info 88, file info 72,
directory info 104, a 64-slot interface table) and the adapter follows the
behavior of WinFsp's public sources (`fsop.c`, `dirctl.c`, `create.c`): for
example, WinFsp does not add `.` and `..`, and a case-insensitive volume
returns a normalized name. No text was copied; a review found no run of seven
or more matching words. Layouts and flag values necessarily agree with the
header. Only public repository files were read.

## 3. Rust dependencies

`cargo deny check licenses` is a gate in `tools/check.sh` (policy in
`deny.toml`). Allowed: MIT, Apache-2.0, BSD-2-Clause, BSD-3-Clause, ISC,
Unicode-3.0, Zlib and GPL-2.0-only. Everything else is rejected, including
GPL-3.0\*, AGPL\* and LGPL-3.0\* (a temporary GPL-3.0-only crate was used to
confirm the rejection). Direct dependencies include `libloading` (ISC), `clap`,
`windows-sys` and `proptest` (MIT OR Apache-2.0), and `bindgen` (BSD-3-Clause,
build tool only; it is not used on WinFsp headers).
