# XFS backend

`backends/xfs` mounts XFS v5 file systems read-write. It falls back to
read-only when a feature or option demands it (see "Write support"). This page
is the operating summary: what it is, how to mount, what can go wrong, how it
is tested and how fast it is.

## Architecture

```
winuxfs.exe (Rust: CLI, Vfs, WinFsp adapter)
   |  C ABI v1 (include/winuxfs/backend.h)
winuxfs_backend_xfs.dll  (C, statically linked, mingw / gcc)
   |-- entry_stub.c / ops.h      wfs_backend_desc, operation table, errno mapping
   |-- mount.c options.c         superblock + feature policy, geometry, option parser
   |-- logcheck.c                log head/tail search: clean or dirty
   |-- buf.c bio.c               buffer cache (single-flight reads, LRU, verifiers), log bio on the host block layer
   |-- icache.c inomap.c         inode cache, XFS ino <-> ABI ino
   |-- dir.c read.c statfs.c     lookup/readdir, read/readlink, statfs
   |-- wops.c wdata.c            namespace operations, data path
   |-- shim/                     Linux kernel API surface the vendored code needs
   `-- third_party/linux-xfs/    kernel fs/xfs, byte for byte (patches in
                                 third_party/linux-xfs-patches/)
```

The kernel's own `libxfs` and log code is compiled against a small shim
(`backends/xfs/shim/`, described in `xfs-shim.md`), so on-disk parsing, CRC32C
checks, verifiers, transactions, the log and recovery are the kernel's. The
backend adds the mount sequence, the caches, the log check, the data path and
the ABI adapter.

`WFS_CAP_READONLY` is not declared; the mount decides at run time (`want_rw`
in `mount.c`) and reports a read-only volume otherwise. Capabilities are
`HARDLINK | SYMLINK`. The backend is thread-safe (no `SINGLE_THREADED`) and
`forget` never does I/O.

Build: `make -C backends/xfs plugin` (host) or
`make -C backends/xfs CROSS=x86_64-w64-mingw32- plugin` (Windows DLL in
`target/plugins-win/`); `tools/build.sh [linux|windows]` does both.

## Mounting

```
winuxfs mount X: <device> --type xfs [--partition N] [--log DEV] [--rt DEV] [--options STR]
```

`<device>` is `disk:N`, `\\.\PhysicalDriveN`, a VHD/VHDX or an image file; a
disk with a partition table needs `--partition`. `--log` and `--rt` give the
external log and realtime devices. `--type auto` probes all backends.

### Options (`--options`, comma separated)

| Option | Meaning |
|---|---|
| `norecovery` | Mount a file system whose log is dirty (or that has `NEEDSREPAIR`) without replaying it. Read-only; metadata may be stale or inconsistent; summary counters are recomputed from the AG headers. An external log device may then be absent. |
| `bufcache_bytes=N` | Buffer cache limit, 1 MiB..64 GiB, default 64 MiB. |
| `inodecache_entries=N` | Inode cache limit, 64..2^24 entries, default 16384. |
| `commit_interval_ms=N` | Period of the kernel log worker (10..3600000 ms, default 30000): how long a change that nobody synced can stay only in memory. It is a process-wide kernel global, so with several mounts the last one wins. A crash loses at most this much; the file system stays consistent. `fsync` and `sync` do not wait for it. |

Unknown options and bad values fail the mount with a message. Default memory
is about 130 MiB (buffers 64 MiB plus inodes, plus constants). File data is not
cached by the backend.

### What is accepted

v5 (CRC) file systems with any block size 1024..65536 and sector size
512..32768, bigtime, nrext64, reflink, rmapbt, finobt, sparse inodes, parent
pointers, external log, a realtime device (data is readable) and `METADIR` (the
root tree is read; the metadata directory is not opened). Rejected: v4,
`ZONED`, unknown incompat bits, `sb_inprogress`. Extended attributes and ACLs
are validated but not exposed (ABI v1 has no xattr operation); the Windows
security descriptor is a per-volume constant.

## Write support

Supported: create, mkdir, unlink, rmdir, rename, link, symlink, write (direct
IO with unwritten-extent ordering), truncate, setattr (including mtime and
ctime on overwrite), log writing and recovery, and rmapbt, reflink-metadata and
parent-pointer updates. The kernel's own background machinery runs unmodified
(`xfsaild`, the log worker, the CIL push workqueue). Replay of a dirty log
written by the Linux kernel is supported, and Linux replays a log left by a
Windows session.

Not supported: reflink copy-on-write, quotas, realtime-device writes,
extended attributes and ACLs. There is no `dirty_limit_bytes`: file data is
never cached (direct IO) and dirty metadata is bounded by the log size, so
there is nothing to limit.

A read-write mount is demoted to read-only (the reason is printed) for:
`norecovery`, a read-only host device, a missing log device, unknown ro-compat
bits, a realtime device, quota accounting, and `METADIR`.

**Reflink-shared inodes.** Without copy-on-write, an inode flagged `REFLINK`
refuses write, shrinking truncate, last-unlink and rename-over with
`EOPNOTSUPP` (`wdata.c`, `wops.c`). This was only tested with the flag set
through `xfs_db`, not with real shared extents.

**Windows.** The shim's `rw_semaphore` is a reader-preferring SRWLOCK +
CONDITION_VARIABLE: an SRWLOCK is writer-fair, and the kernel nests read locks,
which deadlocks. Do not replace it with a bare SRWLOCK. `crashdbg_win.c` prints
the exception code and module+RVA frames to stderr when the plugin crashes.

### Write ordering and cost

A write into a hole allocates an unwritten extent, writes the data, then
converts the extent. There is no device flush per write: the log write of the
checkpoint that carries the conversion starts with a device flush (`PREFLUSH`,
the kernel's `XLOG_ICL_NEED_FLUSH`), so the data is durable before the
conversion can be. `test-crash` verifies this: disabling that `PREFLUSH` in
`bio.c` produces bad crash images. `make bench-write [IMG=...] [OPTS=...]`
prints files/s and MiB/s with device write and flush counts (file-backed
device, so it measures the backend's own cost, not the disk).

### Write tests

All use images made with `WFEAT` (the mkfs feature string, default `PLAIN`);
`-san` variants run ASan/UBSan.

| Target (`make -C backends/xfs ...`) | What |
|---|---|
| `test-wops test-wdata test-wlink test-recover` | namespace ops, data path, links, log recovery |
| `test-crash` (`WCRASH_POINTS=N`) | records device writes and flushes, replays every prefix, checks that `xfs_repair` is clean |
| `test-wstress`, `test-wstress-tsan` | parallel writers; TSan uses `tests/tsan.supp` |
| `test-wrmap`, `test-wpptr` | the suites on a rmapbt+reflink image and on one with parent pointers (`WFEAT=...`); `test-recover` also changes kernel-written parent-pointer images (`root-fixtures/sparse`, `reflink`) and `xfs_repair -n` checks every pointer |
| `test-recover` | also replays a log written by the real kernel (`root-fixtures/dirty-log`) and checks that the background commit (`commit_interval_ms`) makes an unsynced change survive a crash |
| `test-wfuzz-all` | mutates metadata sectors after a write workload; read-write mount including replay, ASan/UBSan, CRC-skip build (env `WFUZZ_ITERS/SEED/MUT/ONLY/STRICT`) |

`tests/tsan.supp` lists only the kernel's intentional lock-free reads, per
function, each with a reason. A new race report must be fixed, not suppressed,
unless it is the same kind of lockless read of a word the kernel also reads
unlocked. Fuzz detection power was not checked with a deliberately planted bug.

## Error mapping

The backend returns `-(Linux errno)`; the core turns that into `Errno`, and the
WinFsp adapter into an `NTSTATUS`.

| Condition | Backend | Windows |
|---|---|---|
| CRC failure, bad on-disk value, `NEEDSREPAIR`, `sb_inprogress`, dirty log, unreadable log | `-EUCLEAN` (117) | `STATUS_FILE_CORRUPT_ERROR` |
| Unsupported feature (v4, zoned, unknown incompat) | `-EOPNOTSUPP` | mount fails, CLI exit 2 |
| Required log or realtime device missing | `-ENODEV` | mount fails, CLI exit 2 |
| Block read failed in the host | `-EIO` | `STATUS_UNEXPECTED_IO_ERROR` |
| Stale inode number from the host | `-ESTALE` | `STATUS_UNEXPECTED_IO_ERROR` |
| Missing name | `-ENOENT` | `STATUS_OBJECT_NAME_NOT_FOUND` |
| Not a directory / is a directory | `-ENOTDIR` / `-EISDIR` | `STATUS_NOT_A_DIRECTORY` / `STATUS_FILE_IS_A_DIRECTORY` |
| `readlink` of something that is not a symlink | `-EINVAL` | (not reachable from Windows) |
| Write on a read-only mount | `EROFS` | `STATUS_MEDIA_WRITE_PROTECTED` |

A corrupt file or directory fails only that operation; the mount stays up. The
details (which check failed, hex masks) go to the backend log, which the CLI
prints on stderr. A dirty log on a read-only mount produces:

```
[backend error] log is dirty (head 0x5 tail 0x0): mount the filesystem on Linux once to replay the log, or use the mount option norecovery to read it without replay (data may be stale)
winuxfs: Backend 'xfs' could not mount disk:1 (error 117): check --type, --partition and --options.
```

## Limits and known behavior

- Log replay happens at a read-write mount. A read-only mount of a dirty log is
  refused unless `norecovery` is given.
- Symlinks appear as regular files whose size is the target length (the
  adapter has no reparse points). Reading one returns the target text, the
  same bytes as `readlink`, like a Cygwin-style plain-file symlink. FIFOs,
  sockets and device nodes appear as empty regular files and read as 0 bytes.
  Real reparse points and path translation of Linux link targets are not
  implemented.
- File names are arbitrary bytes on disk. They are mapped to UTF-16 with
  `crates/winuxfs-core/src/namemap.rs`: invalid UTF-8 bytes become
  U+F700+byte, and reserved characters and trailing dots or spaces become
  U+F000+code. 255 bytes is the maximum name length; paths over 260 characters
  need `\\?\` in programs that do not enable long paths.
- Windows sees no mode bits, link counts or ownership. Hard-link counts and
  link identity are not exposed.
- A file with millions of extents is read completely into memory when opened;
  the inode cache limit is soft for such a file.
- The extent map of a file is held in memory while it is open. Extents of a
  bmbt must be strictly ordered and non-overlapping (kernel patch 0003), so a
  crafted sibling cycle fails with `-EUCLEAN` instead of growing without limit.
  The backend sets a process-wide metadata allocation cap (see below).

## Allocation failure

The shim allocator (`shim/wfs_mem.c`) never returns memory it cannot give: a
request over the cap or a failed `malloc` returns NULL. Kernel call sites that
use `__GFP_NOFAIL` dereference the result without a check, so for those (and
only those) the allocator serves the request above the cap, up to a reserve of
cap/8 + 64 MiB. If that is exhausted, or `malloc` itself fails, the process is
genuinely out of memory: the allocator logs `out of memory (GFP_NOFAIL ...)`
through the host log and calls `abort()`. The cap and the per-inode extent
ceiling in `icache.c` keep a hostile image away from this point.

## Tests

Everything below runs from `tools/check.sh` (host checks first; the Windows
stage with `--vm`).

| What | How |
|---|---|
| Fixture images (16-run orthogonal matrix of block size, sector, dir block, reflink, rmapbt, bigtime, nrext64, metadir, external log, RT; plus hole/unwritten) | `tools/gen-fixtures.sh [--out DIR]` (3-4 min, ~1.6 GB sparse), details in `tests/xfs/README.md` |
| Manifests (ground truth, SHA-256 of every file) | `tools/xfs_manifest.py dir PATH`, `... diff A B` |
| Backend unit tests (shim, buffer cache incl. TSan, mount, log check, inode cache, statfs, dir, read; ASan/UBSan variants) | `make -C backends/xfs test-shim test-buf tsan-buf test-mount test-logcheck test-icache test-statfs test-dir test-read` (add `-san`) |
| Core-level walk of every matrix image against its manifest | `cargo test --workspace` |
| mingw build | `make -C backends/xfs CROSS=x86_64-w64-mingw32- plugin` |
| Fuzzing and mutation tests | harness and runner in `backends/xfs/fuzz/`, wired into `tools/check.sh`; crashing inputs are minimized into `tests/xfs/corpus/` as permanent regression tests |
| Concurrency and cache pressure | `backends/xfs/tests` (ASan/UBSan and TSan variants) |
| Windows acceptance | `tools/check.sh --vm` (below) |

### Windows acceptance

`tools/check.sh --vm` ends with the XFS stage:

1. `tools/xfs_vm_acceptance.py prepare` builds one GPT disk
   (`target/xfs-vm/disk.img`, about 560 MB on disk, sparse) with five
   partitions: 1 the clean matrix image `m00-b1024-s512-ddef-plain`, 2 a copy
   with a dirty log (a start-transaction record whose tail is mkfs's unmount
   record, same layout as `backends/xfs/tests/test_logcheck.c`), 3
   `hole-unwritten.img`, 4 a 768 MiB image with one 512 MiB file, 5 the write
   image.
2. The VM is restored to `clean`, the disk attached with `vmctl.py
   attach-disk`, and `winuxfs.exe`, `winuxfs_backend_xfs.dll` and
   `tools/xfs-vm-acceptance.ps1` are pushed to `C:\t15`.
3. The script mounts each partition on `Y:` with `winuxfs mount`, walks the
   drive, writes a manifest (same schema as `xfs_manifest.py`, hashes from
   `Get-FileHash`, names mapped back through the winuxfs name mapping and
   written in Python's surrogateescape form) plus `results.json`, and
   unmounts.
4. The host compares them with `xfs_vm_acceptance.py verify`. Windows shows no
   mode bits or link counts, so the comparison covers kind, size and SHA-256
   (a symlink is a file of target length; its content is not read).

| Test | Checks |
|---|---|
| `xfs_tree_matches_manifest_on_windows` | whole tree of the clean image, the hole/unwritten image and the perf image equal their manifests |
| `korean_and_nonascii_names_visible` | Korean names, a 255-byte ASCII and a 255-byte Korean name, a Latin-1 (non-UTF-8) name, with matching hashes |
| `file_over_4gib_hash_matches` | `sparse/big_gt4g.bin` (4 GiB + 4 KiB + 11 bytes, mostly hole) hashes equal |
| `dir_3000_entries_enumerates` | 3000 unique entries from `Directory.GetFileSystemEntries` and `Get-ChildItem` |
| `symlink_reported` | the three symlinks are listed with size = target length |
| `dirty_image_refused_with_message` | mount of the dirty image exits nonzero, message names `norecovery` and the log |
| `norecovery_option_mounts` | the same image mounts with `--options norecovery` and equals the manifest |
| `readonly_attribute_and_write_refused` | on a `--options ro` mount the volume is `ReadOnly`; overwrite, append, create, mkdir, delete, rename and set-attribute all fail; `tiny.txt` is unchanged |
| `windows_write_roundtrip` | partition 5 (320 MiB rmapbt/reflink image with parent pointers) mounted read-write: create, overwrite, append, rename, copy, truncate, delete, many files, Korean name, rmdir, 40 MiB file, setattr-then-write; checked live and after remount |
| `xfs_repair_clean_after_windows_writes` | `xfs_repair -n` on the attached data disk after the run (`--data-disk`) |

Set `XFS_FIXTURES` to the fixture directory (default `tests/xfs/images`,
generated on demand), and `WINUXFS_VMCTL` in a git worktree.

## Performance baseline

Measured in the test VM (4 vCPU, 4 GB, Windows 11 build 26300, WinFsp 2.1).
The disk is a file on the host's btrfs, so the media is the host page cache
and the numbers measure the software stack, not a disk. Image
`m00-b1024-s512-ddef-plain` (300 MiB, 1024-byte blocks, 4 KiB directory
blocks), debug build of `winuxfs.exe` (no optimization), release DLL (`-O2`).

| Measure | Result | Notes |
|---|---|---|
| Sequential read, 512 MiB file, 1 MiB reads (.NET `FileStream`) | 800-1100 MiB/s | the backend alone reads at about 6 GiB/s in-process (`make -C backends/xfs bench-dir`), so the cost is WinFsp, the adapter and the debug build |
| 3000-entry directory, first listing (`Directory.GetFileSystemEntries`, cold caches) | 650-920 ms (234 ms when the host had the image cached) | about 100 `blk_read`s (1.6 MiB) for the inodes; the time is device latency, not CPU |
| same, second listing | 19-38 ms | |
| same with `Get-ChildItem` | 83-196 ms | |
| Backend CPU for the 3000 entries (native, `bench-dir`) | readdir 0.6 ms; lookup+forget for all 3000: 6-7 ms cold, 1.4-2.3 ms warm | about 0.8 us per entry |
| First mount (process start to `Y:\` visible) | 3.5-8.9 s right after the VM boots; 48-120 ms on every later mount | the cold number is Windows loading `winuxfs.exe`, the DLL and WinFsp for the first time (a bare `list-partitions` took 9.7 s then), not the backend; the backend's own mount takes below 1 ms with cached blocks |
| Whole-tree walk with `Get-FileHash` (3193 entries incl. a 4 GiB mostly-hole file) | 26-29 s | dominated by hashing 4 GiB |

Notes:

- `readdir` does a lookup, getattr and forget per entry. This is not a
  bottleneck: natively it takes 0.8 us per entry warm and about 2 us cold, so
  the ABI needs no dirent attributes. The cold first listing is bound by about
  100 small device reads, which metadata readahead could merge.
- `AlignedDevice` (`crates/winuxfs-core/src/block/aligned.rs`) does not take
  its lock for reads, and a sector-aligned read goes straight to the inner
  device. Unaligned reads still allocate one temporary span.

Targets, with about 2x headroom over the baseline for the debug build and for
slower hosts. A regression past a target is a bug to look at, not an automatic
gate.

| Measure | Target |
|---|---|
| Sequential read through WinFsp | at least 400 MiB/s on a cached image; the backend alone (`bench-dir`) at least 1 GiB/s |
| 3000-entry directory, warm | at most 100 ms (`GetFileSystemEntries`), 300 ms (`Get-ChildItem`) |
| 3000-entry directory, cold | at most 2 s; at most 150 `blk_read`s |
| Mount, warm OS | at most 500 ms from process start to visible drive; backend `mount` at most 50 ms of CPU on a cached image |
| Mount, first after boot | at most 15 s (OS load, not the backend) |

Re-measure with `tools/check.sh --vm` (it prints a `PERF` line) and
`make -C backends/xfs bench-dir IMG=<image> DIR=/many [FILE=/big.bin]`.
