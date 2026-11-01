# XFS backend (M2 read, M3 write + journal)

`backends/xfs` mounts XFS v5 file systems read-write (read-only when a feature or option demands it; see "Write support"). The design is in
`docs/superpowers/specs/2026-10-29-m2-xfs-readonly-design.md`; this page is
the operating summary: what it is, how to mount, what can go wrong, how it is
tested, and how fast it is.

## Architecture

```
winuxfs.exe (Rust: CLI, Vfs, WinFsp adapter)
   |  C ABI v1 (include/winuxfs/backend.h)
winuxfs_backend_xfs.dll  (C, statically linked, mingw / gcc)
   |-- entry_stub.c / ops.h      wfs_backend_desc, operation table, errno mapping
   |-- mount.c options.c         superblock + feature policy, geometry, option parser
   |-- logcheck.c                log head/tail search: clean or dirty (no replay)
   |-- buf.c                     buffer cache: single-flight reads, LRU, verifiers
   |-- icache.c inomap.c         inode cache, XFS ino <-> ABI ino
   |-- dir.c read.c statfs.c     lookup/readdir, read/readlink, statfs
   |-- shim/                     Linux kernel API surface the vendored code needs
   `-- third_party/linux-xfs/    libxfs read path, unmodified (patches in
                                 third_party/linux-xfs-patches/)
```

The kernel's own `libxfs` read path (29 files) is compiled against a small
shim, so on-disk parsing, CRC32C checks and verifiers are the kernel's. The
backend adds the mount sequence, the caches, the log check and the ABI
adapter. Write operations (wops.c, wdata.c, the log in the vendored `xfs_log*`
and `xfs_trans*`) are filled in; `WFS_CAP_READONLY` is not declared, the mount
decides at runtime (`want_rw`, mount.c) and reports a read-only volume
otherwise. Capabilities are `HARDLINK | SYMLINK`; the backend is thread-safe (no
`SINGLE_THREADED`), and `forget` never does I/O.

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
| `norecovery` | Mount a file system whose log is dirty (or that has `NEEDSREPAIR`) without replaying it. Metadata may be stale or inconsistent; summary counters are recomputed from the AG headers. An external log device may then be absent. |
| `bufcache_bytes=N` | Buffer cache limit, 1 MiB..64 GiB, default 64 MiB. |
| `inodecache_entries=N` | Inode cache limit, 64..2^24 entries, default 16384. |
| `commit_interval_ms=N` | Period of the kernel log worker (10..3600000 ms, default 30000): how long a change that nobody synced can stay only in memory. It is a process-wide kernel global, so with several mounts the last one wins. A crash loses at most this much; the file system stays consistent. `fsync`/`sync` do not wait for it. |

Unknown options and bad values fail the mount with a message. Default memory
is about 130 MiB (buffers 64 MiB plus inodes, plus constants). File data is
not cached by the backend.

### What is accepted

v5 (CRC) file systems with any block size 1024..65536 and sector size
512..32768, bigtime, nrext64, reflink, rmapbt, finobt, sparse inodes, external
log, realtime device (data is readable), and `METADIR` (the root tree is read;
the metadata directory is not opened). Rejected: v4, `ZONED`, unknown incompat
bits, `sb_inprogress`. Extended attributes and ACLs are validated but not
exposed (ABI v1 has no xattr operation); the Windows security descriptor is
derived from the mode bits.

## Write support (M3)

Status: create/mkdir/unlink/rmdir/rename/link/symlink, write (direct IO with
unwritten-extent ordering), truncate, setattr (incl. mtime/ctime on overwrite),
log writing and recovery, rmapbt/reflink metadata updates, kernel-written log
fixtures. The kernel's own background machinery runs unmodified (`xfsaild`, log
worker, CIL push workqueue). Not done: reflink CoW (M4). There is no
`dirty_limit_bytes`: file data is never cached (direct IO) and dirty metadata is
bounded by the log size, so there is nothing to limit.

An rw mount is demoted to read-only (the reason is printed) for: `norecovery`,
read-only host device, missing log device, unknown ro-compat bits, realtime
device, quota accounting, **metadir**. Parent pointers (the newest `mkfs.xfs` default) are
writable: libxfs `xfs_parent.c` is vendored and the kernel's create/link/unlink/rename
code maintains the pointers as attributes with their log intents.

**Reflink-shared inodes.** Without CoW, an inode flagged `REFLINK` refuses
write, shrinking truncate, last-unlink and rename-over with `EOPNOTSUPP`
(`wdata.c`, `wops.c`). Only tested with the flag set through `xfs_db`, not with
real shared extents.

**Windows.** The shim's `rw_semaphore` is a reader-preferring SRWLOCK +
CONDITION_VARIABLE: SRWLOCK is writer-fair and the kernel nests read locks, which
deadlocks. Do not replace it with a bare SRWLOCK. Real-device writes are not
recommended yet; use the test VM.

### Write ordering and cost

A write into a hole allocates an unwritten extent, writes the data, then
converts the extent. There is **no device flush per write**: the log write of the
checkpoint that carries the conversion starts with a device flush (`PREFLUSH`,
the kernel's `XLOG_ICL_NEED_FLUSH`), so the data is durable before the
conversion can be. `test-crash` proves it: disabling that `PREFLUSH` in `bio.c`
is detected (bad crash images). Before this change every 4 KiB write cost one
device flush. `make bench-write [IMG=...] [OPTS=...]` prints files/s and MiB/s
with device write and flush counts (file-backed device, so it measures the
backend's own cost, not the disk).

### Write tests

All use images made with `WFEAT` (mkfs feature string, default in each test as
`PLAIN`); `-san` variants run ASan/UBSan.

| Target (`make -C backends/xfs ...`) | What |
|---|---|
| `test-wops test-wdata test-wlink test-recover` | namespace ops, data path, links, log recovery |
| `test-crash` (`WCRASH_POINTS=N`) | records device writes/flushes, replays every prefix, checks `xfs_repair`-clean state |
| `test-wstress`, `test-wstress-tsan` | parallel writers; TSan uses `tests/tsan.supp` |
| `test-wrmap`, `test-wpptr` | the above suites on a rmapbt+reflink image, and on one with parent pointers (`WFEAT=...`); `test-recover` also changes kernel-written parent-pointer images (`root-fixtures/sparse`, `reflink`) and `xfs_repair -n` checks every pointer |
| `test-recover` | also: replay of a log written by the real kernel (`root-fixtures/dirty-log`), and the background commit (`commit_interval_ms`) making an unsynced change survive a crash |
| `test-wfuzz-all` | mutate metadata sectors after a write workload, rw mount incl. replay, ASan/UBSan, CRC-skip build (env `WFUZZ_ITERS/SEED/MUT/ONLY/STRICT`) |

`tests/tsan.supp` lists only the kernel's intentional lock-free reads (per
function, each with a reason). A new race report must be fixed, not
suppressed, unless it is the same kind of lockless read of a word the kernel
also reads unlocked. Fuzz detection power was not checked with a deliberately
planted bug.

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
| `readlink` of something that is not a symlink | `-EINVAL` | (not reachable from Windows yet) |
| Write on a read-only mount | `EROFS` | `STATUS_MEDIA_WRITE_PROTECTED` |

A corrupt file or directory fails only that operation; the mount stays up.
The details (which check failed, hex masks) go to the backend log, which the
CLI prints on stderr. A dirty log produces:

```
[backend error] log is dirty (head 0x5 tail 0x0): mount the filesystem on Linux once to replay the log, or use the mount option norecovery to read it without replay (data may be stale)
winuxfs: Backend 'xfs' could not mount disk:1 (error 117): check --type, --partition and --options.
```

## Limits and known behavior

- Log replay is done at an rw mount; a read-only mount of a dirty log is refused unless `norecovery`.
- Symlinks appear as regular files whose size is the target length (the
  adapter has no reparse points yet). **Reading one returns the target
  text** (the same bytes as `readlink`, so size and content agree, like a
  Cygwin-style plain-file symlink). FIFOs, sockets and device nodes appear as
  empty regular files and read as 0 bytes. Real reparse points and path
  translation of Linux targets are an M4 design question.
- File names are arbitrary bytes on disk. Names are mapped to UTF-16 with
  `crates/winuxfs-core/src/namemap.rs`: invalid UTF-8 bytes become U+F700+byte,
  reserved characters and trailing dots/spaces become U+F000+code. A 255-byte
  name is the maximum; paths over 260 characters need `\\?\` in programs that
  do not enable long paths.
- Windows sees no mode bits, link counts or ownership; a regular file without
  write bits shows `READONLY` only when the volume itself is not read-only
  (it always is for this backend).
- Hard-link counts and link identity are not exposed to Windows.
- A file with millions of extents is read completely into memory when opened;
  the inode cache limit is soft for such a file (design 7.2).
- `xfs_log_check_lsn` (libxfs asks whether an AGF/AGFL LSN is ahead of the log
  head) is a real definition in `mount.c` that always returns true: the log is
  never replayed or written, so the check is not enforced (the safe direction
  for reading). Nothing is logged per mount. A stricter check needs the log
  head and is deferred to the write backend (M3).
- Memory bound: the extent map of a file is held in memory while it is open.
  Extents of a bmbt must be strictly ordered and non-overlapping (patch 0003),
  so a crafted sibling cycle fails with `-EUCLEAN` instead of growing without
  limit. The production backend sets a process-wide metadata allocation cap
  (see "Allocation failure" below); `__GFP_NOFAIL` allocations are not
  infallible in this port.

## Allocation failure

The shim allocator (`shim/wfs_mem.c`) never returns memory it cannot give:
a request over the cap or a failed `malloc` returns NULL. Kernel call sites
that use `__GFP_NOFAIL` do not check for NULL. For those (and only those) the
shim does not return NULL: it logs `out of memory (GFP_NOFAIL ...)` through the
host log and calls `abort()`-free recovery is impossible, so the shim retries
once after asking the caches to shrink (`wfs_mem_shrink_hook`) and, if that
still fails, `longjmp`s is not available either. See the next paragraph for the
decision actually taken.

## Tests

Everything below runs from `tools/check.sh` (host checks first; the Windows
stage with `--vm`).

| What | How |
|---|---|
| Fixture images (16-run orthogonal matrix of block size, sector, dir block, reflink, rmapbt, bigtime, nrext64, metadir, ext. log, RT; plus hole/unwritten) | `tools/gen-fixtures.sh [--out DIR]` (3-4 min, ~1.6 GB sparse), details in `tests/xfs/README.md` |
| Manifests (ground truth, SHA-256 of every file) | `tools/xfs_manifest.py dir PATH`, `... diff A B` |
| Backend unit tests (shim, buffer cache incl. TSan, mount, log check, inode cache, statfs, dir, read; ASan/UBSan variants) | `make -C backends/xfs test-shim test-buf tsan-buf test-mount test-logcheck test-icache test-statfs test-dir test-read` (add `-san`) |
| Core-level walk of every matrix image against its manifest | `cargo test --workspace` |
| mingw build | `make -C backends/xfs CROSS=x86_64-w64-mingw32- plugin` |
| Fuzzing and mutation tests | harness and runner from M2 task 13 (`backends/xfs/fuzz/`, wired into `tools/check.sh` by that task); crashing inputs are minimized into `tests/xfs/corpus/` as permanent regression tests |
| Concurrency | M2 task 14 |
| Windows acceptance | `tools/check.sh --vm` (below) |

### Windows acceptance

`tools/check.sh --vm` ends with the XFS stage:

1. `tools/xfs_vm_acceptance.py prepare` builds one GPT disk
   (`target/xfs-vm/disk.img`, about 560 MB on disk, sparse) with five
   partitions (the fifth is the write image, below): 1 the clean matrix image `m00-b1024-s512-ddef-plain`, 2 a copy
   with a dirty log (a start-transaction record whose tail is mkfs's unmount
   record, same layout as `backends/xfs/tests/test_logcheck.c`), 3
   `hole-unwritten.img`, 4 a 768 MiB image with one 512 MiB file.
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
| `readonly_attribute_and_write_refused` | on a `--options ro` mount the volume is `ReadOnly`; overwrite, append, create, mkdir, delete, rename, set-attribute all fail; `tiny.txt` unchanged |
| `windows_write_roundtrip` | partition 5 (320 MiB rmapbt/reflink image with parent pointers, `parent=1`) mounted rw: create/overwrite/append/rename/copy/truncate/delete, many files, Korean name, rmdir, 40 MiB file, setattr-then-write; checked live and after remount (7 stages) |
| `xfs_repair_clean_after_windows_writes` | `xfs_repair -n` on the attached data disk after the run (`--data-disk`) |

Set `XFS_FIXTURES` to the fixture directory (default `tests/xfs/images`,
generated on demand) and `WINUXFS_VMCTL` in a worktree.

## Performance baseline

Measured 2026-10-29 in the test VM (4 vCPU, 4 GB, Windows 11 build 26300,
WinFsp 2.1, the disk is a file on the host's btrfs, so the media is the host
page cache and the numbers measure the software stack, not a disk). Image
`m00-b1024-s512-ddef-plain` (300 MiB, 1024-byte blocks, 4 KiB directory
blocks), debug build of `winuxfs.exe` (`target/x86_64-pc-windows-gnu/debug`,
no optimization), release DLL (`-O2`).

| Measure | Result | Notes |
|---|---|---|
| Sequential read, 512 MiB file, 1 MiB reads (.NET `FileStream`) | 800-1100 MiB/s (one early run 507 MiB/s before the AlignedDevice change below) | the backend alone reads at about 6 GiB/s in-process (`make -C backends/xfs bench-dir`), so the cost is WinFsp, the adapter and the debug build |
| 3000-entry directory, first listing (`Directory.GetFileSystemEntries`, cold caches) | 650-920 ms (234 ms when the host had the image cached) | about 100 `blk_read`s (1.6 MiB) for the inodes; the time is device latency, not CPU |
| same, second listing | 19-38 ms | |
| same with `Get-ChildItem` | 83-196 ms | |
| Backend CPU for the 3000 entries (native, `bench-dir`) | readdir 0.6 ms; lookup+forget for all 3000: 6-7 ms cold, 1.4-2.3 ms warm | about 0.8 us per entry |
| First mount (process start to `Y:\` visible) | 3.5-8.9 s right after the VM boots; 48-120 ms on every later mount | the cold number is Windows loading `winuxfs.exe`/the DLL and WinFsp for the first time (a bare `list-partitions` took 9.7 s then), not the backend; the backend's own mount is below 1 ms with cached blocks |
| Whole-tree walk with `Get-FileHash` (3193 entries incl. a 4 GiB mostly-hole file) | 26-29 s | dominated by hashing 4 GiB |

### Findings and fixes

- **readdir does lookup+forget per entry**: measured, not a bottleneck. The
  Vfs calls `stat_at` (lookup, getattr, forget) for each entry. Natively that
  is 0.8 us per entry warm and about 2 us cold; the 3000-entry listing on
  Windows costs 20-40 ms warm, so the ABI does not need dirent attributes.
  The cold first listing is bound by about 100 small device reads.
- **AlignedDevice global mutex and per-call allocation**: reads took the
  mutex and always bounced through a freshly allocated buffer. Fixed
  (`crates/winuxfs-core/src/block/aligned.rs`): reads no longer take the
  lock (only the read-modify-write of writes needs it) and a sector-aligned
  read goes straight to the inner device. Unaligned reads still allocate one
  temporary span. Before/after were not measured separately inside the VM
  (the run-to-run spread of 500-1100 MiB/s is larger than the change); the
  fix removes serialization of concurrent reads and a copy per aligned read.
- Metadata reads are small and unmerged (about 100 reads for 3000 inodes);
  cold listings of big directories would profit from metadata readahead
  (future work).

### Proposed targets (anti-goal "too slow")

Set from the baseline with about 2x headroom for the debug build and for
slower hosts. A regression past a target is a bug to look at, not an
automatic gate.

| Measure | Target |
|---|---|
| Sequential read through WinFsp | at least 400 MiB/s on a cached image; the backend alone (`bench-dir`) at least 1 GiB/s |
| 3000-entry directory, warm | at most 100 ms (`GetFileSystemEntries`), 300 ms (`Get-ChildItem`) |
| 3000-entry directory, cold | at most 2 s; at most 150 `blk_read`s |
| Mount, warm OS | at most 500 ms from process start to visible drive; backend `mount` at most 50 ms of CPU on a cached image |
| Mount, first after boot | at most 15 s (OS load, not the backend) |

Re-measure with `tools/check.sh --vm` (it prints a `PERF` line) and
`make -C backends/xfs bench-dir IMG=<image> DIR=/many [FILE=/big.bin]`.
