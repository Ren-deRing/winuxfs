#!/usr/bin/env python3
"""Scan *.c / *.h files under a directory for SPDX-License-Identifier lines.

Reads target files strictly as data. Prints JSON {"relative/path": "<SPDX or null>"}.
With --summary, prints per-license file counts instead.

Usage: python3 -I tools/license-scan.py [--summary] DIR
"""

import json
import os
import re
import sys
from collections import Counter

HEAD_LINES = 5
SPDX_RE = re.compile(r"SPDX-License-Identifier:\s*(.+?)\s*(?:\*/)?\s*$")

# Deprecated SPDX ids mapped to their current equivalents.
NORMALIZE = {
    "GPL-2.0": "GPL-2.0-only",
}


def normalize(spdx):
    return NORMALIZE.get(spdx, spdx)


def file_license(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for _ in range(HEAD_LINES):
            line = f.readline()
            if not line:
                break
            m = SPDX_RE.search(line)
            if m:
                return normalize(m.group(1))
    return None


def scan(root):
    result = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if not name.endswith((".c", ".h")):
                continue
            full = os.path.join(dirpath, name)
            if not os.path.isfile(full) or os.path.islink(full):
                continue
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            result[rel] = file_license(full)
    return result


def main(argv):
    args = argv[1:]
    summary = False
    if args and args[0] == "--summary":
        summary = True
        args = args[1:]
    if len(args) != 1 or not os.path.isdir(args[0]):
        print(__doc__, file=sys.stderr)
        return 2
    result = scan(args[0])
    if summary:
        counts = Counter("(none)" if v is None else v for v in result.values())
        out = {"total": len(result), "counts": dict(counts.most_common())}
        print(json.dumps(out, indent=2))
    else:
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
