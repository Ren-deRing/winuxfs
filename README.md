# winuxfs

A framework for mounting Linux file systems on Windows 11 x64 through WinFsp.
The framework is Rust; file system backends are C plugins loaded at run time
through a versioned C ABI (`include/winuxfs/backend.h`, `docs/abi.md`).
The first real backend, a port of Linux XFS, is Milestone 2. Milestone 1 (this
tree) is the foundation: block devices (physical disk, VHD/VHDX, image file),
partition-table scanning, the backend ABI and plugin loader, a path-level VFS
layer, name mapping, the WinFsp adapter, the `winuxfs` command, a test
backend (`memfs`) and the Windows test VM.

```
winuxfs mount X: <disk | VHD | image file> --type <backend> [--log <dev>] [--rt <dev>]
winuxfs mount Y: none --type memfs
winuxfs unmount Y:
winuxfs list-backends | list-partitions <device>
```

See `docs/architecture.md` for the layering and the Milestone 1 limits.

## Build

Host: Fedora, Rust stable (`rust-toolchain.toml`), a C compiler.

```sh
tools/build.sh            # Linux: cargo build + backend plugins (target/plugins)
tools/build.sh windows    # cross-build: x86_64-pc-windows-gnu, needs mingw-w64
```

Windows binaries need the `x86_64-pc-windows-gnu` rust target and
`x86_64-w64-mingw32-gcc`. `winuxfs.exe` needs WinFsp 2.x installed on the
Windows machine; it is loaded at run time and is never bundled.

## Test

```sh
tools/check.sh          # fast: fmt, clippy -D warnings, cargo test, python tests, cargo deny
tools/check.sh --vm     # also: Windows cross-build and all Windows e2e tests in the VM
```

The VM is a Windows 11 guest under dockur/windows with rootless podman,
driven by `tools/vmctl.py`; setup and facts are in `docs/vm.md`. Tests that
mount use `Y:` and `X:`, never `Z:` (the VM's share). No Wine is used.
`tools/test_part_xcheck.sh` is skipped unless `sgdisk` is installed.

## License

The project is licensed GPL-2.0-only, to match the Linux XFS code that
Milestone 2 ports (a scan of kernel `fs/xfs` found only GPL-2.0-only,
GPL-2.0-or-later and one LGPL-2.1 file; details in `docs/licensing.md`).
GPL-3.0-only code is not mixed into the project or its binaries, and the
`winfsp` / `winfsp-sys` crates are not used.

WinFsp is not bundled and not linked at build time: `winuxfs-win` declares the
WinFsp API itself and loads `winfsp-x64.dll` from the installed WinFsp at run
time (`LoadLibrary`). The WinFsp license is GPLv3 with a FLOSS exception whose
list of permitted licenses includes GPL-2.0 but whose handling of a
GPL-2.0-only combined work is ambiguous.

**OPEN decision:** which of options A-D in `docs/licensing.md` section 2 to
adopt (A runtime load, nothing bundled, with a notice; B process separation;
C XFS without kernel code; D legal advice). Development proceeds on A, and
nothing is distributed. The final call is the owner's; this is not legal
advice.
