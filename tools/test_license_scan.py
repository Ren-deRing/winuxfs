"""Tests for tools/license-scan.py.

Run from the repo root:  python3 -m unittest tools/test_license_scan.py
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "license-scan.py")


def load_scanner():
    spec = importlib.util.spec_from_file_location("license_scan", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FIXTURES = {
    "a.c": "// SPDX-License-Identifier: GPL-2.0\nint a;\n",
    "b.h": "/* SPDX-License-Identifier: GPL-2.0+ */\n#define B 1\n",
    "c.c": "// SPDX-License-Identifier: GPL-2.0-or-later\nint c;\n",
    "sub/d.c": "/*\n * Copyright x\n */\n// SPDX-License-Identifier: LGPL-2.1\nint d;\n",
    "e.c": "int e;\n",
    "late.c": "\n\n\n\n\n// SPDX-License-Identifier: MIT\n",
    "notes.txt": "SPDX-License-Identifier: MIT\n",
}

EXPECTED = {
    "a.c": "GPL-2.0-only",
    "b.h": "GPL-2.0+",
    "c.c": "GPL-2.0-or-later",
    "sub/d.c": "LGPL-2.1",
    "e.c": None,
    "late.c": None,  # SPDX line beyond the first 5 lines is ignored
}


class LicenseScanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        for rel, text in FIXTURES.items():
            path = os.path.join(self.tmp.name, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)

    def tearDown(self):
        self.tmp.cleanup()

    def test_scan_function(self):
        scanner = load_scanner()
        self.assertEqual(scanner.scan(self.tmp.name), EXPECTED)

    def test_cli_outputs_json(self):
        out = subprocess.run(
            [sys.executable, "-I", SCRIPT, self.tmp.name],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertEqual(json.loads(out), EXPECTED)

    def test_normalize(self):
        scanner = load_scanner()
        self.assertEqual(scanner.normalize("GPL-2.0"), "GPL-2.0-only")
        self.assertEqual(scanner.normalize("GPL-2.0-only"), "GPL-2.0-only")
        self.assertEqual(scanner.normalize("GPL-2.0+"), "GPL-2.0+")


REPO = os.path.dirname(HERE)
VENDOR = os.path.join(REPO, "third_party", "linux-xfs")


class VendoredXfsTest(unittest.TestCase):
    """Gate for third_party/linux-xfs (see docs/licensing.md)."""

    def test_vendored_xfs_files_keep_spdx(self):
        scanner = load_scanner()
        self.assertTrue(os.path.isdir(VENDOR), "third_party/linux-xfs missing")
        found = scanner.scan(VENDOR)
        self.assertGreater(len(found), 100)
        bad = {k: v for k, v in found.items() if v not in scanner.VENDOR_ALLOWED}
        self.assertEqual(bad, {})
        self.assertEqual(scanner.check_allowed(VENDOR), {})

    def test_check_allowed_rejects_gpl3_unknown_and_missing(self):
        scanner = load_scanner()
        with tempfile.TemporaryDirectory() as d:
            for name, text in {
                "ok.c": "// SPDX-License-Identifier: GPL-2.0\n",
                "ok2.h": "/* SPDX-License-Identifier: LGPL-2.1 */\n",
                "g3.c": "// SPDX-License-Identifier: GPL-3.0-or-later\n",
                "g3b.h": "// SPDX-License-Identifier: GPL-3.0\n",
                "mit.c": "// SPDX-License-Identifier: MIT\n",
                "none.c": "int x;\n",
            }.items():
                with open(os.path.join(d, name), "w") as f:
                    f.write(text)
            self.assertEqual(
                sorted(scanner.check_allowed(d)),
                ["g3.c", "g3b.h", "mit.c", "none.c"],
            )
            r = subprocess.run(
                [sys.executable, "-I", SCRIPT, "--check-allowed", d],
                capture_output=True,
                text=True,
            )
            self.assertEqual(r.returncode, 1)
            self.assertIn("g3.c", r.stderr)

    def test_vendored_files_match_recorded_hashes(self):
        import hashlib

        source = os.path.join(VENDOR, "SOURCE")
        self.assertTrue(os.path.isfile(source), "SOURCE missing")
        recorded = {}
        commit = None
        with open(source, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if line.startswith("commit: "):
                    commit = line.split(": ", 1)[1]
                elif line and not line.startswith("#"):
                    digest, path = line.split("  ", 1)
                    recorded[path] = digest
        self.assertEqual(commit, "d135ab453a7400a34930bdda1ef4bdc8acfac80e")
        actual = {}
        for dp, _, names in os.walk(VENDOR):
            for n in names:
                full = os.path.join(dp, n)
                rel = os.path.relpath(full, VENDOR).replace(os.sep, "/")
                if rel == "SOURCE":
                    continue
                with open(full, "rb") as fh:
                    actual[rel] = hashlib.sha256(fh.read()).hexdigest()
        self.assertEqual(actual, recorded)

    def test_vendor_script_verify_passes_and_detects_drift(self):
        script = os.path.join(HERE, "vendor-xfs.sh")
        ok = subprocess.run(["bash", script, "--verify"], capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        with tempfile.TemporaryDirectory() as d:
            shutil.copytree(VENDOR, os.path.join(d, "linux-xfs"))
            victim = os.path.join(d, "linux-xfs", "libxfs", "xfs_sb.c")
            with open(victim, "ab") as f:
                f.write(b"\n")
            env = dict(os.environ, VENDOR_DIR=os.path.join(d, "linux-xfs"))
            bad = subprocess.run(
                ["bash", script, "--verify"], capture_output=True, text=True, env=env
            )
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("xfs_sb.c", bad.stderr)

    def test_vendor_script_refuses_missing_src(self):
        script = os.path.join(HERE, "vendor-xfs.sh")
        r = subprocess.run(
            ["bash", script, "--src", "/nonexistent/xfs"], capture_output=True, text=True
        )
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
