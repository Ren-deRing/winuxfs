#!/usr/bin/env python3
"""Control the winuxfs Windows test VM (dockur/windows under podman).

Subcommands: up, down, kill, run, push, attach-disk, snapshot, restore, status.
See docs/vm.md for the rig layout and the facts this script relies on.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VM_DIR = REPO / "vm"
STORAGE = VM_DIR / "storage"
STORAGE2 = VM_DIR / "storage2"
SHARED = VM_DIR / "shared"
OEM = VM_DIR / "oem"
SNAPSHOTS = VM_DIR / "snapshots"
SSH_DIR = VM_DIR / ".ssh"
SSH_KEY = SSH_DIR / "id_ed25519"

CONTAINER = os.environ.get("WINUXFS_VM_NAME", "winuxfs-vm")
SSH_HOST = "127.0.0.1"
SSH_PORT = 2222
# dockur/windows creates this local administrator (USERNAME default "Docker",
# set explicitly in vm/run.sh). Key auth via administrators_authorized_keys.
SSH_USER = "Docker"

DEFAULT_UP_TIMEOUT = 90 * 60
STOP_TIMEOUT = 120
SNAPSHOT_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


def _quote_cmd_arg(arg: str) -> str:
    """Quote one argument for the Windows side (cmd.exe is sshd's shell)."""
    if arg == "":
        return '""'
    if any(c in arg for c in " \t") and not (arg.startswith('"') and arg.endswith('"')):
        return '"' + arg + '"'
    return arg


def remote_command(argv: list[str]) -> str:
    if len(argv) == 1:
        # A single argument is taken as an already formatted command line.
        return argv[0]
    return " ".join(_quote_cmd_arg(a) for a in argv)


def build_ssh_args(mode: str, argv: list[str]) -> list[str]:
    """Build the ssh argv for `run` (execute argv in the VM) or `probe`."""
    if mode == "run":
        connect_timeout = 10
        command = remote_command(argv)
    elif mode == "probe":
        connect_timeout = 5
        command = "exit 0"
    else:
        raise ValueError(f"unknown ssh mode: {mode}")
    return [
        "ssh",
        "-p", str(SSH_PORT),
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "BatchMode=yes",
        "-o", "IdentitiesOnly=yes",
        "-o", f"ConnectTimeout={connect_timeout}",
        "-i", str(SSH_KEY),
        f"{SSH_USER}@{SSH_HOST}",
        command,
    ]


# ---------------------------------------------------------------- helpers


def _log(msg: str) -> None:
    print(f"vmctl: {msg}", file=sys.stderr, flush=True)


def container_running() -> bool:
    r = subprocess.run(
        ["podman", "container", "inspect", "-f", "{{.State.Running}}", CONTAINER],
        capture_output=True, text=True,
    )
    return r.returncode == 0 and r.stdout.strip() == "true"


def ensure_ssh_key() -> None:
    """Create the rig's dedicated key and install its public half for the VM."""
    SSH_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not SSH_KEY.exists():
        subprocess.run(
            ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "winuxfs-vm",
             "-f", str(SSH_KEY)],
            check=True,
        )
    pub = SSH_KEY.with_suffix(".pub").read_text()
    dest = OEM / "authorized_keys"
    if not dest.exists() or dest.read_text() != pub:
        dest.write_text(pub)


def ssh_ready() -> bool:
    r = subprocess.run(build_ssh_args("probe", []), capture_output=True,
                       stdin=subprocess.DEVNULL)
    return r.returncode == 0


def push(local: Path, remote: str, shared_dir: Path = SHARED) -> Path:
    """Copy `local` to `shared_dir/remote` (\\\\host.lan\\Data\\remote in the VM)."""
    root = shared_dir.resolve()
    dest = (root / remote.replace("\\", "/")).resolve()
    if dest != root and root not in dest.parents:
        raise ValueError(f"remote path escapes the shared folder: {remote}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if Path(local).is_dir():
        shutil.copytree(local, dest, dirs_exist_ok=True)
    else:
        shutil.copy2(local, dest)
    return dest


def _copy_tree(src: Path, dst: Path) -> None:
    # Reflink on btrfs/xfs when possible; keep raw disk images sparse.
    subprocess.run(["cp", "-a", "--reflink=auto", "--sparse=always", str(src), str(dst)],
                   check=True)


# ------------------------------------------------------------- subcommands


def cmd_up(ns: argparse.Namespace) -> int:
    ensure_ssh_key()
    if not (OEM / "winfsp.msi").exists():
        _log("vm/oem/winfsp.msi missing; download it first (see docs/vm.md)")
        return 1
    if not container_running():
        _log(f"starting container {CONTAINER} (first install can take 30-60+ min; "
             "watch http://127.0.0.1:8006)")
        subprocess.run([str(VM_DIR / "run.sh")], check=True)
    deadline = time.monotonic() + ns.timeout
    while True:
        if ssh_ready():
            _log("ssh is up")
            return 0
        if not container_running():
            _log("container exited before ssh came up; see `podman logs` output above")
            return 1
        if time.monotonic() >= deadline:
            _log(f"timed out after {ns.timeout}s waiting for ssh")
            return 1
        time.sleep(15)


def cmd_down(ns: argparse.Namespace) -> int:
    if not container_running():
        return 0
    # dockur turns SIGTERM into an ACPI shutdown and waits for Windows.
    r = subprocess.run(["podman", "stop", "-t", str(STOP_TIMEOUT), CONTAINER])
    return r.returncode


def cmd_kill(ns: argparse.Namespace) -> int:
    return subprocess.run(["podman", "kill", CONTAINER]).returncode


def cmd_status(ns: argparse.Namespace) -> int:
    running = container_running()
    ready = running and ssh_ready()
    print(f"container={'running' if running else 'stopped'} ssh={'up' if ready else 'down'}")
    return 0 if ready else 1


def cmd_run(ns: argparse.Namespace) -> int:
    argv = remote_argv(ns)
    if not argv:
        _log("run: no command given")
        return 2
    return subprocess.run(build_ssh_args("run", argv)).returncode


def cmd_push(ns: argparse.Namespace) -> int:
    dest = push(Path(ns.local), ns.remote)
    rel = dest.relative_to(SHARED.resolve())
    print("\\\\host.lan\\Data\\" + str(rel).replace("/", "\\"))
    return 0


def cmd_attach_disk(ns: argparse.Namespace) -> int:
    image = Path(ns.image)
    if not image.is_file():
        _log(f"no such image: {image}")
        return 1
    rc = cmd_down(ns)
    if rc != 0:
        return rc
    STORAGE2.mkdir(parents=True, exist_ok=True)
    target = STORAGE2 / "data2.img"
    target.unlink(missing_ok=True)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(image), str(target)],
                   check=True)
    return cmd_up(ns)


def cmd_snapshot(ns: argparse.Namespace) -> int:
    if cmd_down(ns) != 0:
        return 1
    dst = SNAPSHOTS / ns.name
    if dst.exists():
        shutil.rmtree(dst)
    SNAPSHOTS.mkdir(parents=True, exist_ok=True)
    _copy_tree(STORAGE, dst)
    _log(f"snapshot {ns.name} saved to {dst} (VM left stopped)")
    return 0


def cmd_restore(ns: argparse.Namespace) -> int:
    src = SNAPSHOTS / ns.name
    if not src.is_dir():
        _log(f"no snapshot named {ns.name}")
        return 1
    if container_running() and cmd_kill(ns) != 0:
        return 1
    # Wait for --rm to remove the container so storage is no longer in use.
    for _ in range(60):
        if subprocess.run(["podman", "container", "exists", CONTAINER]).returncode != 0:
            break
        time.sleep(1)
    if STORAGE.exists():
        shutil.rmtree(STORAGE)
    _copy_tree(src, STORAGE)
    _log(f"restored {ns.name} (VM left stopped; run `vmctl.py up`)")
    return 0


# --------------------------------------------------------------------- cli


def _snapshot_name(value: str) -> str:
    if not SNAPSHOT_NAME_RE.match(value):
        raise argparse.ArgumentTypeError(f"invalid snapshot name: {value!r}")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="vmctl.py", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    up = sub.add_parser("up", help="start the VM and wait for ssh")
    up.add_argument("--timeout", type=int, default=DEFAULT_UP_TIMEOUT,
                    help="seconds to wait for ssh (default: 5400)")
    sub.add_parser("down", help="graceful shutdown")
    sub.add_parser("kill", help="podman kill (crash injection)")
    sub.add_parser("status", help="print container/ssh state; exit 0 if ssh is up")

    run = sub.add_parser("run", help="run a command in the VM: run -- <cmd...>")
    run.add_argument("command", nargs=argparse.REMAINDER)

    push_p = sub.add_parser("push", help="copy a file into vm/shared (\\\\host.lan\\Data)")
    push_p.add_argument("local")
    push_p.add_argument("remote")

    att = sub.add_parser("attach-disk", help="attach a host image as the second disk")
    att.add_argument("image")
    att.add_argument("--timeout", type=int, default=DEFAULT_UP_TIMEOUT)

    for name, helptext in (("snapshot", "save vm/storage as a named snapshot"),
                           ("restore", "replace vm/storage with a named snapshot")):
        sp = sub.add_parser(name, help=helptext)
        sp.add_argument("name", type=_snapshot_name)

    return p.parse_args(argv)


def remote_argv(ns: argparse.Namespace) -> list[str]:
    argv = list(ns.command)
    if argv and argv[0] == "--":
        argv = argv[1:]
    return argv


COMMANDS = {
    "up": cmd_up,
    "down": cmd_down,
    "kill": cmd_kill,
    "status": cmd_status,
    "run": cmd_run,
    "push": cmd_push,
    "attach-disk": cmd_attach_disk,
    "snapshot": cmd_snapshot,
    "restore": cmd_restore,
}


def main(argv: list[str] | None = None) -> int:
    ns = parse_args(sys.argv[1:] if argv is None else argv)
    sys.exit(COMMANDS[ns.cmd](ns))


if __name__ == "__main__":
    main()
