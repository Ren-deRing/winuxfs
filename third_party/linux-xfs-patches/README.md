# linux-xfs-patches

`third_party/linux-xfs/` holds the kernel `fs/xfs` files byte for byte
(`tools/vendor-xfs.sh --verify` fails on any edit). If a kernel file ever has
to change, the change is a patch in this directory, never an edit of the
vendored tree.

Rules:

- Plain `quilt`-style unified diffs, one logical change per file, named
  `NNNN-short-name.patch` (`0001-...`), applied in lexical order. Paths are
  relative to `third_party/linux-xfs/` (`-p1` with `a/` and `b/` prefixes).
- Each patch starts with a header: why it is needed, why a shim cannot do it,
  and whether it is a candidate for upstream.
- Patches are applied by a script (`tools/apply-xfs-patches.sh`) onto a
  copy of the vendored tree in the build directory. The vendored tree itself
  stays pristine, so `--verify` keeps passing.
- Prefer a shim in `backends/xfs/shim/` over a patch; every patch is a
  maintenance cost at each kernel rebase.

- Patch files carry `SPDX-License-Identifier: GPL-2.0` on the first line (the
  patched kernel files are GPL-2.0, so the patches are derivative works).
- Paths in a patch are the file's base name (`--- a/xfs_dir2_leaf.c`); the
  makefile (backends/xfs/mk/dir.mk) patches single files into a generated
  copy, and `tools/apply-xfs-patches.sh` applies each patch inside the
  directory (tree root or `libxfs/`) that holds the file.

Current patches:

- 0001-readdir-no-cookie-mask: xfs_dir2_readdir.c, readdir cookies keep all 32 bits.
- 0002-dir-lookup-bound-leaf-address: xfs_dir2_leaf.c, xfs_dir2_node.c, the leaf
  and node lookups skip an entry whose `address` does not lie inside the data block.
- 0003-bmbt-extent-order: xfs_bmap.c, `xfs_iread_bmbt_block` rejects a record
  that starts before the end of the previous one, so a bmbt sibling cycle
  cannot grow the incore extent map without bound.
- 0004-rmap-merge-len-u64: xfs_rmap.c, the three merge length checks sum in
  64 bits (on Windows `unsigned long` is 32 bits and the sum would wrap).
  Applied once xfs_rmap.c is compiled (M3).

### Reads of `lep->address` that are NOT patched

`xfs_dir2_block.c:749` (block-form directory lookup) forms the entry pointer
from `lep->address` like the two lookups patch 0002 fixes, but needs no patch
today. It relies on two properties, both of which a kernel rebase must keep:

1. `xfs_dir3_block_verify` -> `__xfs_dir3_data_check` runs when the block is read
   and proves that every `bleaf[].address` equals the start of a real data entry
   (an out-of-block or misaligned address fails the verifier with EUCLEAN, so the
   lookup never sees it).
2. Behind the verifier, the buffer cache appends 1 KiB of zeroed slack
   (`WFS_BUF_SLACK`, buf.c) so a name of at most 255 bytes read at the last valid
   entry stays inside the allocation (the over-read is < 300 bytes).

The corpus input `tests/xfs/corpus/bad-block-dir-address-block-end.wsd` (an
address at block end, CRC valid) must fail the walk with EUCLEAN under
`make test-fuzz` (slack = 0, ASan): if a rebase drops property 1 it turns into a
heap over-read and the test fails.

Rebasing to a new kernel commit: update `COMMIT` and the file lists in
`tools/vendor-xfs.sh`, run `tools/vendor-xfs.sh --src <fs/xfs checkout>`,
re-check `docs/licensing.md` counts with
`python3 -I tools/license-scan.py --summary third_party/linux-xfs`, then
re-apply and refresh the patches.
