# Architecture

winuxfs mounts Linux file systems on Windows through WinFsp. A Rust core
(block devices, partition tables, a path-level VFS, the WinFsp adapter, the
`winuxfs` command) loads file system backends, written in C, as plugins
through a versioned C ABI.

## Layers

```
 winuxfs (crates/winuxfs-cli)       mount / unmount / list-backends / list-partitions / service
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
 backend plugin (C)                 backends/xfs, backends/memfs, backends/testplug
```

`winuxfs-conformance` holds a file system conformance suite that runs against
the Vfs over memfs (Linux), a `std::fs` reference (Linux and Windows) and the
mounted drive (Windows VM).

## Rule: the core has no Windows dependency

`winuxfs-core` and `winuxfs-abi` compile and are tested on Linux. All Windows
API use (WinFsp, `CreateFileW` on disks, `VirtDisk`) lives in `winuxfs-win`.
Core code takes `BlockDevice` and byte paths, never Windows handles or UTF-16.

## How a backend attaches

A backend is a DLL exporting `wfs_backend_entry()`, which returns a
`wfs_backend_desc` implementing `include/winuxfs/backend.h` (see `abi.md`).
The core opens the devices (data, and optionally log and realtime: `Devices`)
and exposes them to the backend only through the host API (`blk_size`,
`blk_read`, `blk_write`, `blk_flush`, `log`). It calls `probe` for
`--type auto`, then `mount`. The backend never touches Windows APIs or files
itself. The core wraps the mount in a `Vfs`, which the adapter serves to
WinFsp. The XFS backend is described in `xfs-backend.md`.

## WinFsp

WinFsp is not bundled and not linked at build time. `winuxfs-win` declares the
WinFsp API itself (`winfsp_ffi.rs`) and loads `winfsp-x64.dll` from the
installed WinFsp at run time (`loader.rs`). The `winfsp` and `winfsp-sys`
crates are not used. See `licensing.md`.

## Adapter conventions

- Names: the backend sees raw bytes; the adapter converts through `namemap`.
- `.` and `..` are synthesised by the adapter for non-root directory listings
  (WinFsp does not add them). Backends must not return them from `readdir`.
- readdir cookies from backends must be below 2^63; the adapter uses values
  from 2^63 up for its synthetic entries. A backend cookie of 2^63 or more is
  turned into `EIO`.
- Attributes: only READONLY is stored and mapped; only atime and mtime can be
  set from Windows. Other Windows attributes (hidden, system, archive, reparse
  points) are not supported. The security descriptor is a per-volume constant
  (see below).
- Read-only mounts never reach the backend's write paths (`EROFS` from the
  Vfs); a write call on a read-only block device returns `EROFS` even for
  length 0.
- Extended attributes: the ABI has `WFS_CAP_XATTR`, but the adapter does not
  use it.

### Case sensitivity

Linux file systems are case sensitive and Windows programs are not. With
`--case-insensitive` the Vfs matches names without regard to case: a lookup
first tries the exact name and then falls back to a case-folded search of the
directory. Many programs (game launchers, installers) look files up with a
different case than they were created with and fail without it. When names
collide after folding, the exact match wins. The adapter returns the on-disk
spelling as the normalized name so later requests carry the real name.

### Volume security descriptor

The volume has one security descriptor for every file (`volume_sddl`). By
default it names the mounting user, SYSTEM and Administrators. `--allow-users`
adds Authenticated Users. The service mounts always set it, because their
mounting user is SYSTEM and would otherwise lock out everybody else.

## Drive letters and the Mount Manager

For a drive letter `X:` the adapter first asks WinFsp for the mount point
`\\.\X:`, which registers the volume with the Windows Mount Manager (this needs
administrator rights), and falls back to plain `X:` if that fails.

The registration matters. With a plain drive letter, Windows cannot map the
volume's NT device name (`\Device\Volume{GUID}`) back to the letter:
`GetVolumeNameForVolumeMountPoint` fails with error 1005, `mountvol` does not
list the drive, and `QueryFullProcessImageName` returns
`\Device\Volume{GUID}\...\app.exe` instead of `X:\...\app.exe`. Programs that
rebuild paths from their own image path break (League of Legends passes that
string to CEF, which rejects it as "not an absolute path", and the client
crashes at start-up).

Some programs also open such a path relative to the drive, so the adapter
accepts names that begin with `\Device\Volume{...}` and treats the prefix as
the volume root (`device_prefix_len` in `adapter.rs`).

A directory mount point is not registered this way. Programs cannot be started
from a folder mount (`STATUS_UNRECOGNIZED_VOLUME`); use a drive letter.

## Service mounts

`winuxfs service install` registers a Windows service that mounts the volumes
listed in `winuxfs.conf` at boot (see the README). Details:

- `winuxfs service run` (started by the service manager, LocalSystem,
  auto-start, depends on `WinFsp.Launcher`) runs one `mount` per config line on
  its own thread. A failed mount is retried every 5 s for 2 minutes. Progress
  goes to `winuxfs-service.log` next to the config.
- Stop, shutdown and pre-shutdown call `unmount` for every mount point in
  parallel and wait for the serving threads, so the file system log is flushed
  before Windows goes down (pre-shutdown timeout 60 s).
- Drive letters made by a service are global: they are visible in every
  session. A letter made by an elevated shell exists only for elevated
  processes (UAC split token) and the logon session that made it.
- The mount-point claim objects (`Global\winuxfs-live-<key>`,
  `Global\winuxfs-stop-<key>`) live in the `Global\` namespace so the CLI in
  any session can find them. Their DACL (`D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)`)
  is explicit: the default DACL of an object created by SYSTEM lets
  Administrators only read it, which would make `unmount` fail. `unmount` needs
  an elevated prompt.
- `winuxfs unmount X:` exits with the usage error code and prints "Nothing is
  mounted by winuxfs on X:" when no process serves X: (also when only a stale
  stop event of a killed process remains).

## Tracing

Two environment variables turn on a trace of the WinFsp callbacks (the service
reads them from the machine environment, so set them with `setx /M` and restart
the service):

| Variable | Effect |
|---|---|
| `WINUXFS_TRACE=<file>` | Appends every callback that returns a failure status, with the path (`GetSecurityByName`, `Open`, `Create`, ...). |
| `WINUXFS_TRACE_ALL=1` | Also logs every successful open (`OPEN <handle> opts= acc= size= <path>`), read, create, delete-on-close, can-delete and rename. Large; use for a single run. |

Other variables: `WINUXFS_PLUGIN_PATH` (extra plugin directories),
`WINUXFS_WINFSP_DIR` (absolute path of the WinFsp installation to load
`winfsp-x64.dll` from), and `WINUXFS_LOG` (also print backend info and debug
messages; errors and warnings always go to stderr).

## Operational notes

- **Plugin directory and privileges.** Backend plugins are loaded from
  `<exe>/plugins` (plus any `WINUXFS_PLUGIN_PATH`) into the winuxfs process,
  which is elevated when it mounts a physical disk. A plugin is native code
  that runs with those privileges, so the directory must not be writable by
  unprivileged users: install winuxfs under `C:\Program Files` (or another
  administrator-only location) and do not point `WINUXFS_PLUGIN_PATH` at a
  user-writable directory.
- **Killed process and an offline disk.** Write mounts of a physical disk set
  it offline (non-persistently) and bring it online again when the mount ends
  normally, including Ctrl-C and `winuxfs unmount`. If the process is killed
  (`taskkill /f`, power loss of the session), the disk stays offline and the
  next write mount reports it as already offline without restoring it. Recover
  from an elevated prompt with
  `powershell -Command "Set-Disk -Number N -IsOffline $false"` or, in
  `diskpart`, `select disk N` then `online disk`.
- **Disk numbers.** They change when disks are added or removed. `Get-Disk` is
  the source of truth; check it before mounting a disk for writing.
- **Linux and Windows on one machine.** Windows "Fast Startup" saves the NTFS
  state on shutdown, so Linux mounts the Windows partition read-only. Turn Fast
  Startup off, or restart instead of shutting down.

## Known limitations

- `forget` is called under the global Vfs mutex, including the cache drain at
  `CACHE_MAX`; a slow backend `forget` stalls other requests.
- Case-insensitive directories: an exact-name hit is not cached when names
  collide after folding; a case-only rename is two steps and not atomic.
- A rename during readdir paging may return the renamed entry twice.
- The plugin `SINGLE_THREADED` lock is per loaded plugin object, not per
  dlopen instance; readdir entries with `ino == 0` are not checked.
- Once the CLI's Ctrl handler is installed, Ctrl-C cannot interrupt a blocking
  device open or probe.
- `--log` and `--rt` cannot select a partition.
- Write mode sets a physical disk offline non-persistently (see above);
  4096-byte-sector devices are not exercised end to end; the bounce buffer is
  allocated per I/O.
- Directory mount points cannot run programs (see above).
