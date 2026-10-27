"""Tests for tools/license-scan.py.

Run from the repo root:  python3 -m unittest tools/test_license_scan.py
"""

import importlib.util
import json
import os
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


if __name__ == "__main__":
    unittest.main()
