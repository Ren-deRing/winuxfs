# SPDX-License-Identifier: GPL-2.0-only
"""Exit codes and messages of the winuxfs CLI for permission errors.

Simulates "the device needs administrator rights" with a file the current
user cannot read: list-partitions must exit with 3 and say what to do.
The binary is $WINUXFS_BIN, else target/debug/winuxfs (build it first).

    python3 -m unittest tools/test_cli_errors.py
"""

import os
import pathlib
import stat
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = os.environ.get("WINUXFS_BIN", str(ROOT / "target" / "debug" / "winuxfs"))


class PermissionErrors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A missing binary is a failure, not a silent skip.
        if not os.path.exists(BIN):
            raise AssertionError(f"{BIN} not built: run 'cargo build -p winuxfs-cli' "
                                 "or set WINUXFS_BIN")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dev = pathlib.Path(self.tmp.name) / "locked.img"
        self.dev.write_bytes(b"\0" * 4096)
        self.dev.chmod(0)

    def tearDown(self):
        self.dev.chmod(stat.S_IRUSR | stat.S_IWUSR)
        self.tmp.cleanup()

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root reads any file")
    def test_unreadable_device_exits_3(self):
        r = subprocess.run([BIN, "list-partitions", str(self.dev)],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertIn("locked.img", r.stderr)
        self.assertIn("denied", r.stderr)
        # One sentence that tells the user what to do.
        self.assertRegex(r.stderr, r"(administrator|root|permission)")
        self.assertEqual(r.stdout, "")


if __name__ == "__main__":
    unittest.main()
