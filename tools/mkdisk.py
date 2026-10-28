#!/usr/bin/env python3
"""Build the block-device test pattern image.

Every 4096-byte block starts with its block number as an 8-byte
little-endian integer; the rest of the block is zero. The output is fully
determined by the size (default 64 MiB). Used by the Windows disk E2E tests
(crates/winuxfs-win/tests/e2e_disk.rs) via `tools/vmctl.py attach-disk`.

Usage: python3 -I tools/mkdisk.py [--size BYTES] OUT
"""

import argparse
import struct
import sys

BLOCK = 4096
DEFAULT_SIZE = 64 * 1024 * 1024


def make_image(path, size=DEFAULT_SIZE):
    """Write the pattern image of `size` bytes (a positive multiple of 4096)."""
    if size <= 0 or size % BLOCK:
        raise ValueError(f"size must be a positive multiple of {BLOCK}: {size}")
    tail = bytes(BLOCK - 8)
    with open(path, "wb") as f:
        for n in range(size // BLOCK):
            f.write(struct.pack("<Q", n))
            f.write(tail)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE, help="bytes")
    ap.add_argument("out")
    ns = ap.parse_args(argv)
    try:
        make_image(ns.out, ns.size)
    except ValueError as e:
        print(f"mkdisk: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
