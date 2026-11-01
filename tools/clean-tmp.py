#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Remove stale test temp dirs from target/tmp.

The XFS tests keep their images on failure (`images kept in ...`) so a failure can be
debugged; they add up to tens of GB. This removes only directories named
test_<name>.<pid> whose pid no longer runs and that are older than --min-age-min
(default 60), by absolute path, one at a time. Nothing else is touched.
usage: clean-tmp.py [TMPDIR] [--min-age-min N]
"""
import os, re, shutil, sys, time

args = [a for a in sys.argv[1:]]
age_min = 60
if "--min-age-min" in args:
    i = args.index("--min-age-min")
    age_min = int(args[i + 1])
    del args[i:i + 2]
base = os.path.abspath(args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "target", "tmp"))
if not os.path.isdir(base):
    sys.exit(0)

def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True

n = freed = 0
now = time.time()
for e in sorted(os.listdir(base)):
    m = re.fullmatch(r"test_\w+\.(\d+)", e)
    p = os.path.join(base, e)
    if not m or not os.path.isdir(p) or os.path.islink(p) or alive(int(m.group(1))):
        continue
    if now - os.stat(p).st_mtime < age_min * 60:
        continue
    for r, _, fs in os.walk(p):
        for f in fs:
            try:
                freed += os.lstat(os.path.join(r, f)).st_blocks * 512
            except OSError:
                pass
    shutil.rmtree(p)
    n += 1
if n:
    print(f"clean-tmp: removed {n} stale test dirs from {base} ({freed / 2**30:.1f} GiB)")
