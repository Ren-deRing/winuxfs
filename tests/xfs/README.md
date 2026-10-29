<!-- SPDX-License-Identifier: GPL-2.0-only -->
# XFS test fixtures

Images are generated, not committed (`tests/xfs/images/`, git-ignored).

    tools/gen-fixtures.sh [--out DIR]    # about 3-4 minutes, ~1.6 GB sparse
    tools/gen-fixtures.sh --self-test    # one small image, a few seconds (run by tools/check.sh)
    tools/gen-fixtures.sh --list         # matrix names

Needs xfsprogs (`mkfs.xfs`, `xfs_db`, `xfs_repair`, `xfs_logprint`); exit code
77 and an install hint when missing. No root, no mounts. Every image must pass
`xfs_repair -n` and have a CLEAN log, otherwise the script fails.

Per image `NAME`: `NAME.img` (+ `NAME.log.img`, `NAME.rt.img` for external
devices; pass them to `xfs_db -l/-R`, `xfs_repair -l/-r`),
`NAME.manifest.json` (`tools/xfs_manifest.py dir` of the source tree) and
`NAME.expect.json` (xfs_db: all superblock fields, root ino, log state, and per
path: ino, mode, size, fork format, extent count, nblocks, directory form
`shortform|block|leaf|node`, bmbt level, extent list when <= 64 extents).

## Manifest

`tools/xfs_manifest.py dir [--skip-hash-over BYTES] PATH` and
`... diff A.json B.json` (exit 0 equal, 1 different). Directory nlink is
`2 + subdirectories`; directory sizes and timestamps are not recorded; files
over the skip limit have `sha256: "skipped"` (matrix images: > 1 TiB, i.e.
never; hole image: > 1 GiB, i.e. `far.bin`).

## Matrix

16 runs of the Hadamard array L16(2^15), see the comment in `gen-fixtures.sh`:
block size 1024/4096/65536, sector 512/4096, directory block default/64K,
reflink, rmapbt, bigtime, nrext64, metadir, external log, RT device. Any pair
of binary factors meets in all four combinations. Deviations:

- Sector 4096 cannot go with block size 1024 (mkfs refuses): clamped to 512 in
  those runs, so (1024, 4096) is not covered.
- The three block sizes use two array columns, so 4096 takes two of the four
  combinations: 8 runs of 4096, 4 of 1024, 4 of 65536.
- RT with reflink or rmapbt needs `metadir=1` (mkfs: "reflink not supported on
  realtime devices without metadir"): those runs are promoted to metadir.
- The default directory block is max(block size, 4096) in mkfs, so 1024-byte
  block images have 4096-byte directory blocks unless d64k.

Every source tree has: empty, 1-byte, exactly one block, block+1, multi-block
files; extents-format file (8 extents) and btree files with 64 and many extents;
holes (head, tail, middle; `sparse/big_gt4g.bin` is 4 GiB + 4 KiB + 11 bytes);
directories `dir_sf`, `dir_block`, `dir_leaf`, `many` (3000 entries), and
`many_xl` (20000, only when the dir block is 64K, to reach the node form there);
Korean names, a 255-byte ASCII name, a 255-byte Korean name, a non-UTF-8
(Latin-1) name; local, 300-byte remote and dangling symlinks; hard links; a
FIFO; mode bits. Look in `expect.json` for the actual forms in each image.

Known limits of `mkfs.xfs -p` (7.1.1) observed here:

- Holes shorter than 4096 bytes are not preserved (copy uses page-size data
  units), so hole geometry is in units of max(block size, 4096).
- Files are never "inline": regular file data is always in blocks; only
  directories and symlinks have a local fork. "Inline" size = tiny files.
- "bmbt 2-level" (root level 2) is reached at block sizes 1024 and 4096 only;
  at 65536 the data of that many extents would not fit in a 300 MiB image, so
  there it reaches level 1 (one leaf level, root in inode).
- A RT device is created but empty: `-p` cannot create realtime files
  ("creating realtime files from proto file not supported"). RT file data
  comes from the root fixtures. `xfs_db` paths cannot contain a space, so the
  trees avoid them.
- `mkfs.xfs` prints harmless "Transaction block reservation exceeded" lines
  for some 64K-directory images; `xfs_repair -n` is clean.
- Not creatable here: dirty logs, reflink-shared extents, device nodes,
  kernel-written fragmentation, files on the RT device.

## Hole / unwritten image (`hole-unwritten.img`)

`mkfs.xfs -m crc=1,reflink=0,rmapbt=0 -p` of a tree with `isl.bin` (blocks
0,2,4 = A,B,C), `whole.bin` (4 blocks of W), `hole.bin` (native holes,
8 blocks), `far.bin` ("near" at 0, "far" at 1<<40), `tiny.txt`. Then:

    xfs_db -x -c 'path /isl.bin'   -c 'write u3.bmx[1].extentflag 1' IMG
    xfs_db -x -c 'path /whole.bin' -c 'write u3.bmx[0].extentflag 1' IMG

`NAME.manifest.json` describes what a reader must see (unwritten extents read
as zeros: `isl.bin` block 2 and all of `whole.bin`), not the on-disk bytes.
With rmapbt on, `xfs_repair -n` reports "Incorrect reverse-mapping ... should
be ... unwritten" for these two files; rmapbt is therefore off and the image is
clean, so no warning has to be tolerated.

## Root-only fixtures

`tools/gen-root-fixtures.sh` (run once as root by a person, never by tests):
sparse, unwritten, fragmented, reflink, device nodes, dirty log, external log +
RT (two variants). Output `tests/xfs/root-fixtures/*.img.zst` is committed.
The script refuses to run unless root, uses only new loop devices on temp
files and cleans up on exit. It has not been run on this host.

### Dirty-log coverage gap (Task 7)

`backends/xfs/tests/test_logcheck.c` builds its dirty logs by synthesizing log
records into `mkfs.xfs` images (no kernel mount is possible without root), and
cross-checks the verdict with `xfs_logprint -t`. What that cannot prove: that a
record written by the real Linux kernel (header layout, CRC over header +
extension headers + payload, cycle stamping, lsunit-sized records) is accepted
byte for byte. mkfs writes a zero record CRC and `xfs_logprint -t` does not
verify CRCs, so no host tool is an independent oracle for the CRC either.
Needs root fixture `tests/xfs/root-fixtures/dirty-log.img.zst` from
`tools/gen-root-fixtures.sh`; once committed, the test `root_fixture_dirty_log`
(skipped with a message while the file is absent) must report the image dirty
(`-EUCLEAN`) and mount it with `norecovery`.
