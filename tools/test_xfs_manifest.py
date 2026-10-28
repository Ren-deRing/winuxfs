# SPDX-License-Identifier: GPL-2.0-only
"""Tests for tools/xfs_manifest.py.

Run from the repo root:  python3 -m unittest tools.test_xfs_manifest -v
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

from tools import xfs_manifest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "xfs_manifest.py")


def make_tree(root):
    os.makedirs(os.path.join(root, "sub"))
    with open(os.path.join(root, "a.txt"), "wb") as f:
        f.write(b"hello")
    with open(os.path.join(root, "sub", "b.bin"), "wb") as f:
        f.write(bytes(range(256)))
    os.link(os.path.join(root, "a.txt"), os.path.join(root, "sub", "a.link"))
    os.symlink("a.txt", os.path.join(root, "sym"))
    os.mkfifo(os.path.join(root, "fifo"))
    with open(os.path.join(root, "한글"), "wb") as f:
        f.write(b"x")


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "src")
        os.mkdir(self.root)
        make_tree(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def test_manifest_is_deterministic(self):
        a = xfs_manifest.dump(xfs_manifest.build(self.root))
        b = xfs_manifest.dump(xfs_manifest.build(self.root))
        self.assertEqual(a, b)
        keys = list(json.loads(a)["entries"])
        self.assertEqual(keys, sorted(keys))
        self.assertIn("/a.txt", keys)

    def test_diff_detects_changed_hash_and_missing_file(self):
        before = xfs_manifest.build(self.root)
        with open(os.path.join(self.root, "a.txt"), "wb") as f:
            f.write(b"HELLO")  # same size, different hash
        os.unlink(os.path.join(self.root, "sub", "b.bin"))
        after = xfs_manifest.build(self.root)
        self.assertEqual(xfs_manifest.diff(before, before), [])
        out = "\n".join(xfs_manifest.diff(before, after))
        self.assertIn("/a.txt", out)
        self.assertIn("sha256", out)
        self.assertIn("/sub/b.bin", out)
        self.assertIn("missing", out)
        # CLI exit codes
        pa = os.path.join(self.tmp.name, "a.json")
        pb = os.path.join(self.tmp.name, "b.json")
        for p, m in ((pa, before), (pb, after)):
            with open(p, "w") as f:
                f.write(xfs_manifest.dump(m))
        r = subprocess.run([sys.executable, "-I", SCRIPT, "diff", pa, pa])
        self.assertEqual(r.returncode, 0)
        r = subprocess.run([sys.executable, "-I", SCRIPT, "diff", pa, pb],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("/a.txt", r.stdout)

    def test_symlink_and_hardlink_recorded(self):
        e = xfs_manifest.build(self.root)["entries"]
        self.assertEqual(e["/sym"]["kind"], "symlink")
        self.assertEqual(e["/sym"]["target"], "a.txt")
        self.assertEqual(e["/a.txt"]["nlink"], 2)
        self.assertEqual(e["/sub/a.link"]["nlink"], 2)
        self.assertEqual(e["/sub/a.link"]["hardlink_of"], "/a.txt")
        self.assertNotIn("hardlink_of", e["/a.txt"])
        self.assertEqual(e["/a.txt"]["sha256"],
                         hashlib.sha256(b"hello").hexdigest())
        self.assertEqual(e["/fifo"]["kind"], "fifo")
        self.assertEqual(e["/sub"]["kind"], "dir")
        self.assertEqual(e["/sub"]["nlink"], 2)
        self.assertEqual(e["/"]["nlink"], 3)  # '.', parent, 'sub'/'..'.
        self.assertEqual(e["/"]["kind"], "dir")
        self.assertIn("/한글", e)

    def test_sparse_hash_matches_dense_hash(self):
        p = os.path.join(self.root, "sparse.bin")
        with open(p, "wb") as f:
            f.write(b"head")
            f.seek(3 << 20)
            f.write(b"mid")
            f.truncate(5 << 20)  # trailing hole
        dense = hashlib.sha256()
        with open(p, "rb") as f:
            dense.update(f.read())
        e = xfs_manifest.build(self.root)["entries"]["/sparse.bin"]
        self.assertEqual(e["sha256"], dense.hexdigest())
        e = xfs_manifest.build(self.root, skip_hash_over=1 << 20)["entries"]
        self.assertEqual(e["/sparse.bin"]["sha256"], "skipped")
        self.assertNotEqual(e["/a.txt"]["sha256"], "skipped")


if __name__ == "__main__":
    unittest.main()
