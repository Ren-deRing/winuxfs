# winuxfs

Mount Linux file systems on Windows 11 x64 through
[WinFsp](https://github.com/winfsp/winfsp). The framework is Rust; file system
backends are C plugins loaded at run time through a versioned C ABI
(`include/winuxfs/backend.h`, `docs/abi.md`).

winuxfs is an independent project. It is not related to winuxos or affiliated with its authors.

- **XFS backend**: a port of the Linux kernel's XFS (v5), read-write, with log
  replay, reflink/rmapbt/parent-pointer metadata and external log devices
  (`docs/xfs-backend.md`).
- **Devices**: physical disks (`disk:N`, `\\.\PhysicalDriveN`), VHD/VHDX and
  image files, with MBR/GPT/extended partition scanning.
- **Windows integration**: drive letters registered with the Mount Manager,
  case-insensitive matching for Windows programs, and a Windows service that
  mounts volumes at boot.
- Test backends: `memfs` and a few plugins for ABI tests.

```
winuxfs mount X: <disk | VHD | image file> --type <backend> [--partition N] [--log <dev>] [--rt <dev>]
                 [--options <str>] [--ro] [--case-insensitive] [--allow-users]
winuxfs mount Y: none --type memfs
winuxfs unmount Y:
winuxfs list-backends | list-partitions <device>
winuxfs service install | uninstall      (mounts from winuxfs.conf at boot, see below)
```

Run `winuxfs mount --help` for all options. `winuxfs mount` needs an elevated
prompt when it opens a physical disk, and keeps running until Ctrl-C or
`winuxfs unmount`.

Typical use of an XFS partition on the second disk:

```
winuxfs mount Y: disk:1 --partition 1 --type xfs --case-insensitive
```

Use `--case-insensitive` for programs such as game launchers: XFS is case
sensitive, and Windows programs often open files with a different case than
they were created with. Use a drive letter, not a folder: programs cannot be
started from a folder mount.

Check disk numbers with `Get-Disk` before mounting for writing. They change
when disks are added or removed, and a write mount takes the disk offline
until it is unmounted.

## Mounting at boot (Windows service)

`winuxfs service install` (administrator) registers the `winuxfs` service. It
reads `winuxfs.conf` next to `winuxfs.exe`, one mount per line, written like the
arguments of `winuxfs mount`:

```
# winuxfs.conf
Y: disk:1 --partition 1 --type xfs --case-insensitive
```

Then `sc.exe start winuxfs` (or reboot; it starts with Windows). Because the
service runs as SYSTEM, the drive letter is visible in every session (File
Explorer, Steam, other users), unlike a mount made from an elevated shell,
which a non-elevated Explorer does not see. Service mounts are open to all
signed-in users (as `--allow-users` does for a manual mount; without it only
the mounting user, SYSTEM and Administrators can open the volume). Stopping the
service or shutting Windows down unmounts cleanly, so the file system log is
flushed. A mount that fails (for example because the disk is not ready yet) is
retried every 5 s for 2 minutes. Progress and errors go to
`winuxfs-service.log` next to the config. After editing the config, run
`sc.exe stop winuxfs` and `sc.exe start winuxfs`. `winuxfs unmount Y:` also
works from an elevated prompt.

## Build

Host: Fedora, Rust stable (`rust-toolchain.toml`), a C compiler.

```sh
tools/build.sh            # Linux: cargo build + backend plugins (target/plugins)
tools/build.sh windows    # cross-build: x86_64-pc-windows-gnu, needs mingw-w64
```

Windows binaries need the `x86_64-pc-windows-gnu` Rust target and
`x86_64-w64-mingw32-gcc`. `winuxfs.exe` needs WinFsp 2.x installed on the
Windows machine; it is loaded at run time and never bundled.

## Test

```sh
tools/check.sh          # fast: fmt, clippy -D warnings, cargo test, python tests, cargo deny
tools/check.sh --vm     # also: Windows cross-build and all Windows e2e tests in the VM
```

The VM is a Windows 11 guest under dockur/windows with rootless podman, driven
by `tools/vmctl.py`; setup and facts are in `docs/vm.md`. Tests that mount use
`Y:` and `X:`, never `Z:` (the VM's share). No Wine is used.
`tools/test_part_xcheck.sh` is skipped unless `sgdisk` is installed.

## Documentation

| File | Content |
|---|---|
| `docs/architecture.md` | layers, adapter conventions, drive letters and the Mount Manager, service, tracing, limitations |
| `docs/abi.md` | the backend ABI |
| `docs/xfs-backend.md` | XFS backend: mounting, options, write support, errors, tests, performance |
| `docs/xfs-shim.md` | the kernel API shim under the XFS backend |
| `docs/vm.md` | the Windows test VM |
| `docs/licensing.md` | license decisions |

## License

GPL-2.0-only, see `LICENSE`. It matches the Linux XFS code the backend is
ported from (the kernel's `fs/xfs` files are GPL-2.0-only, GPL-2.0-or-later and
one LGPL-2.1 file; see `docs/licensing.md`). GPL-3.0-only code is not mixed
into the project or its binaries, and the `winfsp` and `winfsp-sys` crates are
not used.

WinFsp is not bundled and not linked at build time: `winuxfs-win` declares the
WinFsp API itself and loads `winfsp-x64.dll` from the installed WinFsp at run
time (`LoadLibrary`). WinFsp is GPLv3 with a FLOSS exception that permits
linking from software under a free license such as GPL-2.0-only, provided the
notice below is kept and WinFsp is not combined with proprietary software.

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
<https://github.com/winfsp/winfsp>
