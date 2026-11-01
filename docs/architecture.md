# Architecture (Milestone 1)

## Layers

```
 winuxfs (crates/winuxfs-cli)       mount / unmount / list-backends / list-partitions
        |
 winuxfs-win                        Windows only: WinFsp loader + FFI, adapter,
        |                           WinDisk (\\.\PhysicalDriveN), VHD/VHDX, device specs
        |  Vfs (Arc)                BlockDevice impls for Windows
 winuxfs-core                       no Windows dependency (builds and tests on Linux)
   vfs        path-level API, inode cache, case folding, open-handle table
   plugin     dlopen/LoadLibrary of backends, safe Mount wrapper, error sanitising
   hostapi    wfs_host_api callbacks backed by BlockDevice
   block      BlockDevice trait, FileDevice, MemDevice, AlignedDevice
   part       MBR / GPT / extended partitions -> PartitionView
   namemap    disk bytes <-> UTF-16 (reserved chars to private-use area)
        |  C ABI v1 (include/winuxfs/backend.h, crates/winuxfs-abi)
 backend plugin (C)                 backends/memfs (M1), backends/testplug, XFS (M2)
```

`winuxfs-conformance` holds a filesystem conformance suite run against the
Vfs over memfs (Linux), a `std::fs` reference (Linux and Windows), and the
mounted drive (Windows VM).

## Rule: the core has no Windows dependency

`winuxfs-core` and `winuxfs-abi` must compile and be tested on Linux. All
Windows API use (WinFsp, `CreateFileW` on disks, `VirtDisk`) lives in
`winuxfs-win`. Core code takes `BlockDevice` and byte paths, never Windows
handles or UTF-16.

## How the XFS backend attaches (Milestone 2)

A backend is a DLL exporting `wfs_backend_entry()` that returns a
`wfs_backend_desc` implementing `include/winuxfs/backend.h`. The core opens
the devices (data, and optional log and realtime: `Devices`), exposes them to
the backend only through the host API (`blk_size`, `blk_read`, `blk_write`,
`blk_flush`, `log`), and calls `probe` for `--type auto`, then `mount`. The
backend never touches Windows APIs or files itself. The core wraps the mount
in a `Vfs`, which the adapter serves to WinFsp. XFS code is a C port living
under `backends/xfs`; ported files keep their SPDX lines.

## Adapter conventions

- Names: the backend sees raw bytes; the adapter converts through `namemap`.
- `.` and `..` are synthesised by the adapter for non-root directory listings
  (WinFsp does not add them). Backends must not return them from `readdir`.
- readdir cookies from backends must be below 2^63; the adapter uses values
  from 2^63 up for its synthetic entries. A backend cookie of 2^63 or more
  is turned into `EIO`.
- Attributes: only READONLY is stored and mapped; only atime and mtime can be
  set from Windows. Other Windows attributes (hidden, system, archive,
  reparse points, security descriptors beyond the default) are not supported.
- Read-only mounts never reach the backend's write paths (`EROFS` from the
  Vfs); a write call on a read-only block device returns `EROFS` even for
  length 0.

## Exclusions and deferred items

Not in Milestone 1: XFS itself (Milestone 2); extended attributes (the ABI
has `WFS_CAP_XATTR` but the adapter does not use it); Windows security
descriptors beyond the default; a boot-time service (added later, see
`winuxfs service` in the README).

Known gaps, from the review ledger:

- `forget` is called under the global Vfs mutex, including the cache-drain
  at `CACHE_MAX`; a slow backend `forget` will stall other requests.
- Case-insensitive directories: an exact-name hit is not cached when names
  collide after folding; a case-only rename is two steps and not atomic.
- Rename over a directory: the index drop is untestable with memfs (needs an
  inode-reusing backend).
- A rename during readdir paging may return the renamed entry twice
  (memfs gives it a fresh cookie).
- `rename_onto_open_target_with_replace` accepts either Ok or Err in the
  tests.
- Plugin `SINGLE_THREADED` lock is per loaded plugin object, not per dlopen
  instance; readdir entries with `ino == 0` are not checked.
- Once the CLI's Ctrl handler is installed, Ctrl-C cannot interrupt a blocking
  device open or probe.
- `--log` / `--rt` cannot select a partition.
- Disk access: write mode sets the disk offline non-persistently (a killed
  process leaves it offline until reboot; see "Operational notes" below); 4096-byte-sector devices are not
  exercised end to end; the bounce buffer is allocated per I/O.
- VM rig: `install.bat` still runs `sc query WinFsp`; snapshot/restore are not
  atomic; no checksums for the downloaded MSIs.
- `tools/test_part_xcheck.sh` (sgdisk cross-check) has never run on this host.
- WinFsp license option A-D is undecided (README, `docs/licensing.md`).

## Operational notes

- **Plugin directory and privileges.** Backend plugins are loaded from
  `<exe>/plugins` (plus any `WINUXFS_PLUGIN_PATH`) into the winuxfs process,
  which is elevated when it mounts a physical disk. A plugin is native code
  that runs with those privileges, so the directory must not be writable by
  unprivileged users: install winuxfs under `C:\Program Files` (or another
  administrator-only location) and do not set `WINUXFS_PLUGIN_PATH` to a
  user-writable directory.
- **Killed process and an offline disk.** Write mounts of a physical disk set
  it offline (non-persistently) and bring it online again when the mount ends
  normally, including Ctrl-C and `winuxfs unmount`. If the process is killed
  (`taskkill /f`, power loss of the session), the disk stays offline and the
  next write mount reports it as already offline without restoring it. Recover
  by hand from an elevated prompt:
  `powershell -Command "Set-Disk -Number N -IsOffline $false"` or, in
  `diskpart`, `select disk N` then `online disk`. A `winuxfs online` helper
  is not provided in M1.
- **Service mounts.** `winuxfs service run` (started by the service manager,
  LocalSystem, auto-start, depends on `WinFsp.Launcher`) runs one
  `mount` per config line on its own thread. Stop, shutdown and pre-shutdown
  call `unmount` for each mount point in parallel and wait for the serving
  threads, so the XFS log is flushed before Windows goes down (pre-shutdown
  timeout 60 s). The mount-point claim objects live in the `Global\` namespace
  so the CLI in any session can find them. Their DACL (`D:(A;;GA;;;SY)(A;;GA;;;BA)
  (A;;GA;;;OW)`) is explicit: the default DACL of a SYSTEM-created object lets
  Administrators only read it, which would make `unmount` fail. `unmount` needs
  an elevated prompt. The volume's own descriptor (`volume_sddl`) names the
  mounting user, SYSTEM and Administrators; `--allow-users` (always on for
  service mounts, whose mounting user is SYSTEM) adds Authenticated Users.
- **`unmount` of nothing.** `winuxfs unmount X:` exits with the usage error
  code and prints "Nothing is mounted by winuxfs on X:" when no process serves
  X: (also when only a stale stop event of a killed process remains).
