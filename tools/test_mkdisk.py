# SPDX-License-Identifier: GPL-2.0-only
"""Tests for tools/mkdisk.py.

Run from the repo root:  python3 -m unittest tools/test_mkdisk.py
"""

import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "mkdisk.py")


def load_mkdisk():
    spec = importlib.util.spec_from_file_location("mkdisk", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class MkdiskTest(unittest.TestCase):
    def setUp(self):
        self.mk = load_mkdisk()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def path(self, name):
        return os.path.join(self.tmp.name, name)

    def test_small_image_layout(self):
        out = self.path("small.img")
        self.mk.make_image(out, 3 * 4096)
        with open(out, "rb") as f:
            data = f.read()
        self.assertEqual(len(data), 3 * 4096)
        for n in range(3):
            block = data[n * 4096 : (n + 1) * 4096]
            self.assertEqual(block[:8], struct.pack("<Q", n))
            self.assertEqual(block[8:], bytes(4088))

    def test_default_is_64_mib_pattern(self):
        self.assertEqual(self.mk.DEFAULT_SIZE, 64 * 1024 * 1024)
        out = self.path("default.img")
        r = subprocess.run(
            [sys.executable, "-I", SCRIPT, out], capture_output=True, text=True
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.path.getsize(out), 64 * 1024 * 1024)
        with open(out, "rb") as f:
            for n in (0, 1, 100, 16383):
                f.seek(n * 4096)
                self.assertEqual(f.read(8), struct.pack("<Q", n), n)

    def test_deterministic(self):
        a, b = self.path("a.img"), self.path("b.img")
        self.mk.make_image(a, 40 * 4096)
        self.mk.make_image(b, 40 * 4096)
        with open(a, "rb") as fa, open(b, "rb") as fb:
            ha = hashlib.sha256(fa.read()).hexdigest()
            hb = hashlib.sha256(fb.read()).hexdigest()
        self.assertEqual(ha, hb)

    def test_overwrites_existing_file(self):
        out = self.path("old.img")
        with open(out, "wb") as f:
            f.write(b"\xff" * (5 * 4096))
        self.mk.make_image(out, 2 * 4096)
        self.assertEqual(os.path.getsize(out), 2 * 4096)

    def test_rejects_bad_sizes(self):
        for size in (0, -4096, 4095, 4097):
            with self.assertRaises(ValueError, msg=size):
                self.mk.make_image(self.path("bad.img"), size)
        r = subprocess.run(
            [sys.executable, "-I", SCRIPT, "--size", "1000", self.path("c.img")],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(os.path.exists(self.path("c.img")))


if __name__ == "__main__":
    unittest.main()
