# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for tools/vmctl.py that run without a VM.

Run with: python3 -m unittest tools/test_vmctl.py
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vmctl  # noqa: E402


class BuildSshArgsTest(unittest.TestCase):
    def test_run_builds_full_ssh_command(self):
        args = vmctl.build_ssh_args("run", ["dir", "C:\\"])
        self.assertEqual(args[0], "ssh")
        self.assertEqual(args[1:3], ["-p", "2222"])
        self.assertIn("StrictHostKeyChecking=no", args)
        self.assertIn("BatchMode=yes", args)
        self.assertEqual(args[-2], "Docker@127.0.0.1")
        self.assertEqual(args[-1], "dir C:\\")

    def test_every_option_value_follows_dash_o(self):
        args = vmctl.build_ssh_args("run", ["ver"])
        for opt in ("StrictHostKeyChecking=no", "UserKnownHostsFile=/dev/null", "BatchMode=yes"):
            i = args.index(opt)
            self.assertEqual(args[i - 1], "-o")

    def test_uses_dedicated_identity_file(self):
        args = vmctl.build_ssh_args("run", ["ver"])
        i = args.index("-i")
        self.assertEqual(args[i + 1], str(vmctl.SSH_KEY))
        self.assertIn("IdentitiesOnly=yes", args)

    def test_arguments_with_spaces_are_double_quoted(self):
        args = vmctl.build_ssh_args("run", ["type", "C:\\Program Files\\x.txt"])
        self.assertEqual(args[-1], 'type "C:\\Program Files\\x.txt"')

    def test_single_preformatted_command_is_passed_verbatim(self):
        args = vmctl.build_ssh_args("run", ['dir "C:\\Program Files"'])
        self.assertEqual(args[-1], 'dir "C:\\Program Files"')

    def test_empty_argument_is_quoted(self):
        args = vmctl.build_ssh_args("run", ["echo", ""])
        self.assertEqual(args[-1], 'echo ""')

    def test_probe_runs_exit_and_short_timeout(self):
        args = vmctl.build_ssh_args("probe", [])
        self.assertEqual(args[-1], "exit 0")
        self.assertIn("ConnectTimeout=5", args)

    def test_unknown_mode_raises(self):
        with self.assertRaises(ValueError):
            vmctl.build_ssh_args("bogus", ["ver"])


class CliTest(unittest.TestCase):
    def _main(self, argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
            vmctl.main(argv)
        return cm.exception.code

    def test_unknown_subcommand_exits_2(self):
        self.assertEqual(self._main(["frobnicate"]), 2)

    def test_no_subcommand_exits_2(self):
        self.assertEqual(self._main([]), 2)

    def test_snapshot_requires_name(self):
        self.assertEqual(self._main(["snapshot"]), 2)

    def test_restore_requires_name(self):
        self.assertEqual(self._main(["restore"]), 2)

    def test_parse_run_strips_double_dash(self):
        ns = vmctl.parse_args(["run", "--", "sc", "query", "WinFsp"])
        self.assertEqual(ns.cmd, "run")
        self.assertEqual(vmctl.remote_argv(ns), ["sc", "query", "WinFsp"])

    def test_parse_up_timeout_default_is_90_minutes(self):
        ns = vmctl.parse_args(["up"])
        self.assertEqual(ns.timeout, 90 * 60)
        ns = vmctl.parse_args(["up", "--timeout", "120"])
        self.assertEqual(ns.timeout, 120)

    def test_snapshot_name_rejects_path_separators(self):
        self.assertEqual(self._main(["snapshot", "../evil"]), 2)


class PushTest(unittest.TestCase):
    def test_push_copies_into_shared_dir(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "a.txt"
            src.write_text("hi")
            shared = Path(d) / "shared"
            dest = vmctl.push(src, "sub/b.txt", shared_dir=shared)
            self.assertEqual(dest, shared / "sub" / "b.txt")
            self.assertEqual(dest.read_text(), "hi")

    def test_push_rejects_escape_from_shared_dir(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "a.txt"
            src.write_text("hi")
            with self.assertRaises(ValueError):
                vmctl.push(src, "../x.txt", shared_dir=Path(d) / "shared")


if __name__ == "__main__":
    unittest.main()
