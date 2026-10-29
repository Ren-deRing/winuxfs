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

Current patches: 0001-readdir-no-cookie-mask (applied to xfs_dir2_readdir.c by backends/xfs/mk/dir.mk).

Rebasing to a new kernel commit: update `COMMIT` and the file lists in
`tools/vendor-xfs.sh`, run `tools/vendor-xfs.sh --src <fs/xfs checkout>`,
re-check `docs/licensing.md` counts with
`python3 -I tools/license-scan.py --summary third_party/linux-xfs`, then
re-apply and refresh the patches.
