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
winuxfs service install | uninstall      (mounts from winuxfs.conf at boot, see below)
```

### Mounting at boot (Windows service)

`winuxfs service install` (administrator) registers the `winuxfs` service. It
reads `winuxfs.conf` next to `winuxfs.exe`, one mount per line, written like the
arguments of `winuxfs mount`:

```
# winuxfs.conf
Y: disk:1 --partition 1 --type xfs --case-insensitive
```

Then `sc.exe start winuxfs` (or reboot; it starts with Windows).
Because the service runs as SYSTEM, the drive letter is visible in every session
(File Explorer, Steam, other users), unlike a mount made from an elevated shell,
which a non-elevated Explorer does not see. Service mounts are open to all
signed-in users (Authenticated Users get full access, as `--allow-users` does
for a manual mount; without it only the mounting user, SYSTEM and
Administrators can open the volume). Stopping the service or shutting
Windows down unmounts cleanly (the file system log is flushed). A mount that
fails (disk not ready yet) is retried every 5 s for 2 minutes. Progress and
errors go to `winuxfs-service.log` next to the config. Edit the config, then
`sc.exe stop winuxfs` and `sc.exe start winuxfs`. `winuxfs unmount Y:` works
from an elevated prompt too. Disk numbers can change when disks are added or
removed: check them with `Get-Disk`.

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

License: GPL-2.0-only, see `LICENSE`. The project is licensed GPL-2.0-only, to match the Linux XFS code that
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
