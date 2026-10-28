#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Directory manifests for the XFS fixtures (M2 design 10.3).

  xfs_manifest.py dir [--skip-hash-over BYTES] PATH
                                  print the manifest of PATH as JSON; files
                                  larger than BYTES get sha256 "skipped"
                                  (hashing a 1 TiB sparse file is pointless)
  xfs_manifest.py diff A.json B.json
                                  print differences; exit 0 if none, 1 if any

Entries are keyed by '/'-separated path (root is '/'), keys sorted. Names are
decoded with surrogateescape so non-UTF-8 names survive. Directory sizes and
timestamps are not recorded (they differ between file systems); a
directory's nlink is computed as 2 + its subdirectory count.
"""

import hashlib
import json
import os
import stat
import sys

VERSION = 1


_CHUNK = 1 << 20
_ZEROS = bytes(_CHUNK)


def _sha256(path):
    """SHA-256 of the file content. Holes are fed as zeros without reading
    them (SEEK_DATA/SEEK_HOLE), so large sparse files are cheap to hash."""
    h = hashlib.sha256()
    size = os.stat(path).st_size

    def zeros(n):
        while n > 0:
            k = min(n, _CHUNK)
            h.update(_ZEROS[:k])
            n -= k

    with open(path, "rb") as f:
        fd = f.fileno()
        pos = 0
        while pos < size:
            try:
                data = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:  # ENXIO: only a hole remains (or no support)
                data = size
            data = min(data, size)
            zeros(data - pos)
            if data >= size:
                break
            hole = min(os.lseek(fd, data, os.SEEK_HOLE), size)
            f.seek(data)
            n = hole - data
            while n > 0:
                b = f.read(min(n, _CHUNK))
                if not b:  # file shrank under us
                    zeros(n)
                    break
                h.update(b)
                n -= len(b)
            pos = hole
    return h.hexdigest()


def _entry(path, st, skip_over):
    m = st.st_mode
    e = {"mode": stat.S_IMODE(m), "nlink": st.st_nlink}
    if stat.S_ISDIR(m):
        e["kind"] = "dir"
        # Host file systems disagree about directory link counts (btrfs: 1);
        # record the POSIX/XFS value: 2 + number of subdirectories.
        with os.scandir(path) as it:
            e["nlink"] = 2 + sum(1 for d in it if d.is_dir(follow_symlinks=False))
    elif stat.S_ISLNK(m):
        e["kind"] = "symlink"
        e["target"] = os.fsdecode(os.readlink(path))
        e["size"] = st.st_size
    elif stat.S_ISREG(m):
        e["kind"] = "file"
        e["size"] = st.st_size
        if skip_over is not None and st.st_size > skip_over:
            e["sha256"] = "skipped"
        else:
            e["sha256"] = _sha256(path)
    elif stat.S_ISFIFO(m):
        e["kind"] = "fifo"
    elif stat.S_ISSOCK(m):
        e["kind"] = "socket"
    elif stat.S_ISCHR(m) or stat.S_ISBLK(m):
        e["kind"] = "chr" if stat.S_ISCHR(m) else "blk"
        e["rdev"] = [os.major(st.st_rdev), os.minor(st.st_rdev)]
    else:
        e["kind"] = "other"
    return e


def build(root, skip_hash_over=None):
    """Return the manifest dict of the tree under root."""
    root = os.fspath(root)
    entries = {}
    first_seen = {}  # (dev, ino) -> first path in sorted order

    def visit(abs_path, rel):
        st = os.lstat(abs_path)
        e = _entry(abs_path, st, skip_hash_over)
        if not stat.S_ISDIR(st.st_mode) and st.st_nlink > 1:
            key = (st.st_dev, st.st_ino)
            if key in first_seen:
                e["hardlink_of"] = first_seen[key]
            else:
                first_seen[key] = rel
        entries[rel] = e
        if stat.S_ISDIR(st.st_mode):
            for name in sorted(os.listdir(abs_path)):
                visit(os.path.join(abs_path, name),
                      ("/" + name) if rel == "/" else rel + "/" + name)

    visit(root, "/")
    return {"version": VERSION, "entries": dict(sorted(entries.items()))}


def dump(manifest):
    return json.dumps(manifest, sort_keys=True, indent=1) + "\n"


def diff(a, b):
    """Human-readable difference lines between two manifests ([] if equal)."""
    out = []
    ea, eb = a["entries"], b["entries"]
    for p in sorted(set(ea) | set(eb)):
        if p not in eb:
            out.append(f"{p}: missing in B")
        elif p not in ea:
            out.append(f"{p}: missing in A")
        elif ea[p] != eb[p]:
            for k in sorted(set(ea[p]) | set(eb[p])):
                if ea[p].get(k) != eb[p].get(k):
                    out.append(f"{p}: {k}: {ea[p].get(k)!r} != {eb[p].get(k)!r}")
    return out


def main(argv):
    if len(argv) >= 3 and argv[1] == "dir":
        args, skip = argv[2:], None
        if len(args) == 3 and args[0] == "--skip-hash-over":
            skip, args = int(args[1]), args[2:]
        if len(args) == 1:
            sys.stdout.write(dump(build(args[0], skip)))
            return 0
    if len(argv) == 4 and argv[1] == "diff":
        docs = []
        for p in argv[2:]:
            with open(p, encoding="utf-8") as f:
                docs.append(json.load(f))
        lines = diff(*docs)
        for line in lines:
            print(line)
        return 1 if lines else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
