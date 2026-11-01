# Windows test VM

winuxfs runs its Windows end-to-end tests in a Windows 11 VM provided by
[dockur/windows](https://github.com/dockur/windows) (QEMU/KVM inside a
container), run with rootless podman. `tools/vmctl.py` drives it.

## Quick start

```sh
# one-time: WinFsp 2.x and the Win32-OpenSSH MSI into vm/oem/ (gitignored)
curl -L -o vm/oem/winfsp.msi \
  https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi
curl -L -o vm/oem/openssh.msi \
  https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64-v10.4.0.0.msi

tools/vmctl.py up                 # first run installs Windows: ~10 min from a local ISO
tools/vmctl.py run -- ver
tools/vmctl.py run -- sc query WinFsp.Launcher   # see "WinFsp 2.x service names"
tools/vmctl.py snapshot clean     # once, right after the first install
```

(`gh release download -R winfsp/winfsp -p '*.msi' -D vm/oem/` also works when
`gh` is installed; rename the result to `winfsp.msi`.) `vm/oem/` must contain
both MSIs before the first `up`; `install.bat` does not download them.

The install can be watched in a browser at <http://127.0.0.1:8006>.

## vmctl.py subcommands

| Command | What it does |
|---|---|
| `up [--timeout S]` | Creates `vm/.ssh/id_ed25519` (once) and copies its public key to `vm/oem/authorized_keys`, starts the container via `vm/run.sh` if it is not running, then waits until SSH answers (default 5400 s). |
| `down` | `podman stop -t 120`: dockur turns this into an ACPI shutdown and waits for Windows. The container is `--rm`, so it is removed afterwards. |
| `kill` | `podman kill` (crash injection). |
| `status` | Prints container and SSH state; exit 0 only when SSH is up. |
| `run -- <cmd...>` | Runs the command in the VM over SSH; stdout, stderr and the exit code pass through. sshd's shell is `cmd.exe`. Several arguments are joined with spaces (arguments containing spaces are double-quoted); a single argument is sent verbatim, so `run -- 'dir "C:\Program Files"'` works. |
| `push <local> <remote>` | Copies a file or directory to `vm/shared/<remote>` and prints its UNC path in the VM (`\\host.lan\Data\<remote>`). |
| `attach-disk <image>` | `down`, copy the image to `vm/storage2/data2.img`, `up`. It always overwrites the previous image, so a stale image cannot contaminate a run. |
| `snapshot <name>` | `down`, then copy `vm/storage/` to `vm/snapshots/<name>/` (the VM stays stopped). |
| `restore <name>` | `kill`, then replace `vm/storage/` with `vm/snapshots/<name>/` (the VM stays stopped; run `up`). |

A snapshot takes about 30 s including the graceful shutdown and is reflinked
on btrfs; a restore takes about 2 s. In a git worktree there is no `vm/`
state: set `WINUXFS_VMCTL` to the main checkout's `tools/vmctl.py`.

## Layout

| Host path | Container path | Notes |
|---|---|---|
| `vm/storage/` | `/storage` | Primary disk `data.img` (64G sparse raw), UEFI vars, dockur state. Gitignored. |
| `vm/storage2/` | `/storage2` | Second disk `data2.img`. Mounted only when `data2.img` exists. Gitignored. |
| `vm/oem/` | `/oem` | `install.bat` (tracked), `authorized_keys`, `winfsp.msi`, `openssh.msi` (gitignored). |
| `vm/shared/` | `/shared` | Samba share. Gitignored. |
| `vm/snapshots/` | - | `vmctl.py snapshot` output. Gitignored. |
| `vm/.ssh/` | - | The rig's dedicated SSH key pair. Gitignored. |
| `~/다운로드/Windows11_Client_x64_en-us_26300_9457.iso` | `/custom.iso` (ro) | Local install media; override with `WINUXFS_ISO`. A stripped `tiny11.iso` also installs and passes the XFS write acceptance. |

`vm/run.sh` environment overrides: `WINUXFS_VM_NAME` (default `winuxfs-vm`),
`WINUXFS_VM_IMAGE` (default `docker.io/dockurr/windows:6.06`), `WINUXFS_ISO`,
`WINUXFS_CPU` (default 4), `WINUXFS_RAM` (default `4G`).
`WINUXFS_HOST_DISK=/dev/...` passes a host block device to the guest as a
read-only NVMe disk; with `WINUXFS_HOST_DISK_RW=1` it is read-write. Do not
mount the same device on the host at the same time.

## Facts about the dockur image (6.06)

Sources: the README, `docs/environment.md`, `src/install.sh` and `src/samba.sh`
of dockur/windows, and `src/disk.sh` and `src/network.sh` of qemus/qemu.

**Image.** `docker.io/dockurr/windows` (also `ghcr.io/dockur/windows`), pinned
to tag `6.06`. Required options: `--device=/dev/kvm --device=/dev/net/tun
--cap-add NET_ADMIN`, port 8006 (web viewer) and a 120 s stop timeout.

**CPU and RAM.** `CPU_CORES` (default 2) and `RAM_SIZE` (default `4G`). The rig
uses 4 cores and 4G.

**Account.** dockur creates a local administrator `Docker` with password
`admin` (`USERNAME`/`PASSWORD` environment); `vm/run.sh` sets both explicitly.
SSH logs in as `Docker` with key authentication. Because it is an
administrator, OpenSSH reads the key from
`C:\ProgramData\ssh\administrators_authorized_keys`, not from the user profile.
The SSH session already has a high-integrity token (`net session` succeeds),
so opening `\\.\PhysicalDriveN` needs no `runas` or scheduled task.

**Custom ISO.** Bind-mounting an ISO file at `/custom.iso` skips the download;
`VERSION` is then ignored. dockur detects the edition from the ISO (`Detected:
Windows 11 Pro`, answer file `win11x64.xml`) and boots it with a small overlay
`setup.img` instead of rewriting it, so no 9 GB copy appears in `vm/storage`.
`VERSION=11` stays in `run.sh` only as the fallback when the ISO is absent.

**SELinux and the user's ISO.** The rig's own directories are mounted with
`:Z`. The ISO lives in the user's home and must not be relabeled, so it is
mounted `:ro` without `:Z` and the container runs with `--security-opt
label=disable` so that it can read a `user_home_t` file.

**OEM folder.** `/oem` is copied to `C:\OEM`, and `install.bat` in it runs once,
during the final step of the automatic installation (first logon). It does not
run again on later boots; to re-provision, delete `vm/storage/` or restore a
snapshot taken before provisioning. dockur lints `install.bat` at container
start and prints warnings in `podman logs`. `install.bat` logs to
`C:\OEM\install.log` and copies the log to `vm/shared/oem-install.log` when the
share is reachable.

**OpenSSH.** `install.bat` uses the built-in `OpenSSH.Server` capability if it
is installed, otherwise the bundled Win32-OpenSSH MSI. The MSI is preferred
over `Add-WindowsCapability`, which downloads from Windows Update, is slow and
may not offer features on demand for Insider builds such as 26300.

**Shared folder.** dockur runs Samba in the container and exports `/shared` as
share `Data` on host name `host.lan`. In the VM it is `\\host.lan\Data`, also
mapped as drive `Z:` and a `Shared` desktop shortcut for the logged-on user.
`Z:` is not visible in SSH sessions, so over SSH use the UNC path. Tests mount
on `Y:` or `X:`, never `Z:`.

**Second disk.** `DISK2_SIZE` + a `/storage2` mount makes dockur create
`/storage2/data2.img` (raw; `.qcow2` with `DISK_FMT=qcow2`). An existing image
is attached as-is, never shrunk. If `/storage2` is mounted without an image,
dockur creates a blank one of `DISK2_SIZE` (or `DISK_SIZE`, 64G), so `run.sh`
mounts `/storage2` only when `data2.img` exists. The rig copies the image
instead of bind-mounting it, so the guest cannot modify the original. Disks
use `DISK_TYPE=scsi` by default and appear in Windows as `BusType SAS`.

**Rootless podman networking.** Under rootless podman the default NAT mode
fails silently and dockur falls back to user-mode networking via passt
(`podman logs` shows `Mode: User (passt)`). In user mode only ports in
`USER_PORTS` are forwarded; for Windows the default is RDP only, so `run.sh`
sets `USER_PORTS=22`. Host ports are bound to 127.0.0.1 only: SSH
`127.0.0.1:2222`, web viewer `127.0.0.1:8006`.

**btrfs.** dockur warns that `/storage` on btrfs "might introduce issues with
Windows Setup" and recommends disabling COW (`chattr +C`) on disk images. The
install works without it on the reference host. Snapshots use
`cp --reflink=auto --sparse=always`, which is near-instant on btrfs.

**WinFsp 2.x service names.** There is no service called `WinFsp`, so `sc query
WinFsp` fails with error 1060. WinFsp 2.x installs side by side: the kernel
driver service is named `WinFsp+<SxS id>` (for example
`WinFsp+20261027T233951Z`) and is `STOPPED` until the first file system loads
it on demand. The user-mode launcher service is `WinFsp.Launcher` and runs
after install. To check the install:

```sh
tools/vmctl.py run -- sc query WinFsp.Launcher            # RUNNING
tools/vmctl.py run -- 'sc query type= driver state= all | findstr /i WinFsp'
tools/vmctl.py run -- '"C:\Program Files (x86)\WinFsp\bin\fsptool-x64.exe" ver'
```

The install directory is `C:\Program Files (x86)\WinFsp\` (registry
`HKLM\SOFTWARE\WOW6432Node\WinFsp\InstallDir`). `winfsp-x64.dll` is in `bin\`
and is not on `PATH`: test executables that link it need the DLL next to them,
or they exit with `0xC0000135` (`STATUS_DLL_NOT_FOUND`) and no output.

## Reference environment

Host: Fedora, kernel 7.2.11, podman 5.8.9 rootless, SELinux enforcing, btrfs.
Image `dockurr/windows:6.06` (QEMU 11.1.1). ISO
`Windows11_Client_x64_en-us_26300_9457.iso`: build 26300.9457, an Insider
build that dockur detects as Windows 11 Pro. WinFsp 2.1.

- The first `vmctl.py up` from the local ISO takes about 9 minutes to SSH;
  later boots take about 15 s.
- `install.bat` installs the bundled OpenSSH MSI, WinFsp, `sshd` and
  `WinFsp.Launcher` (both RUNNING); the WinFsp driver loads on demand.
- WinFsp's own `winfsp-tests-x64.exe` (`create_test getfileinfo_test
  rdwr_noncached_test querydir_test delete_test rename_test`) passes.
- `run` propagates the exit code (`run -- exit 7` gives 7).

## Running tests in the VM

### Disk and VHD tests (`crates/winuxfs-win/tests/e2e_disk.rs`)

```sh
python3 -I tools/mkdisk.py /tmp/pattern.img        # 64 MiB, block n starts with n (u64 LE)
tools/vmctl.py attach-disk /tmp/pattern.img        # copies it to vm/storage2/data2.img
cargo test -p winuxfs-win --target x86_64-pc-windows-gnu --no-run
tools/vmctl.py push target/x86_64-pc-windows-gnu/debug/deps/e2e_disk-<hash>.exe t14/e2e_disk.exe
tools/vmctl.py run -- 'copy /y \\host.lan\Data\t14\e2e_disk.exe C:\t14\e2e_disk.exe'
tools/vmctl.py run -- 'C:\t14\e2e_disk.exe'
```

- **Disk numbers.** Disk 0 is the 64 GB system disk (`IsBoot`/`IsSystem`,
  GPT). The attached image is disk 1 (RAW, online). The tests find it by size
  among the non-boot, non-system, non-file-backed disks and require exactly one
  match. `WinDisk::open` refuses write mode on the disk holding the Windows
  volume (`EBUSY`).
- **Offline state.** Write-mode `WinDisk::open` sets the disk offline
  non-persistently and restores it when the device is dropped. If a test
  process is killed in between, the disk stays offline until the next reboot
  (`vmctl.py down`/`up`) or `powershell -Command "Set-Disk -Number 1 -IsOffline
  $false"`.
- **VHDX.** The Hyper-V PowerShell module (`New-VHD`) is not installed, so the
  tests create fixed VHDX files with `diskpart` (`create vdisk ... type=fixed`)
  under `%TEMP%\winuxfs-e2e-disk`. Attachments are tied to the virtual-disk
  handle (no `PERMANENT_LIFETIME`), so a killed test process detaches
  automatically (`vhd_detached_when_process_killed` checks this); each test
  deletes leftover files of an earlier run first.
  `ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST` cannot be used: the disk is then not
  surfaced and `GetVirtualDiskPhysicalPath` fails with error 55.

### `tools/check.sh --vm`

Runs the host checks (`cargo fmt --check`, clippy with `-D warnings`, tests,
the Python tests, `cargo deny check`), then `tools/build.sh windows`, restores
the `clean` snapshot, attaches the 64 MiB pattern disk (`tools/mkdisk.py`),
pushes `winuxfs.exe`, the memfs and testplug DLLs and every test executable of
`winuxfs-win` to `C:\t16`, and runs each one over SSH. Any failure gives a
nonzero exit; the VM is stopped at the end only if the script started it. The
suites are `e2e_cli`, `e2e_conformance`, `e2e_disk`, `e2e_read`, `e2e_write`,
`ffi_layout` and `ffi_smoke`.

`tools/m1-acceptance.bat` is the scripted equivalent of a manual Explorer
check: it mounts memfs with the CLI and exercises it with `cmd.exe` (mkdir,
echo, type, ren, move, del, rmdir, `vol`, `dir /s`) in one logon session, then
runs `winuxfs unmount`.

Afterwards the script restores `clean` again and runs the XFS acceptance
(details and test list in `xfs-backend.md`):

```sh
python3 -I tools/xfs_vm_acceptance.py prepare target/xfs-vm   # GPT disk: clean, dirty-log, hole, perf, write images
tools/vmctl.py restore clean && tools/vmctl.py attach-disk target/xfs-vm/disk.img
tools/vmctl.py push target/vm-stage-xfs t15                   # winuxfs.exe, xfs plugin DLL, xfs-vm-acceptance.ps1
tools/vmctl.py run -- 'xcopy /y /e /i /q \\host.lan\Data\t15 C:\t15'
tools/vmctl.py run -- 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\t15\xfs-vm-acceptance.ps1 -Dir C:\t15 -Out C:\t15\out'
tools/vmctl.py run -- 'xcopy /y /e /i /q C:\t15\out \\host.lan\Data\t15out'   # results land in vm/shared/t15out
python3 -I tools/xfs_vm_acceptance.py verify target/xfs-vm vm/shared/t15out
```

### Behavior seen in the VM

- A drive letter that a process mounts exists only in that process's logon
  session (WinFsp defines it with `DefineDosDevice`), and every SSH connection
  is a new logon session. `vmctl.py run -- dir Y:\` from a second connection
  therefore does not see a mount held by another. The tests run `cmd /c dir
  Y:\` from inside the mounting session instead.
- Processes started through ssh die when the session ends and take their
  WinFsp mount with them. Start long-lived mounts detached:
  `Invoke-CimMethod Win32_Process -MethodName Create` (the drive then lives in
  session 0), or use the `winuxfs service`.
- The first `winuxfs.exe` start after a boot of the restored snapshot is slow
  (3.5-9 s to a visible drive, 9.7 s for a bare `list-partitions`); later
  mounts take 50-120 ms. Do not read the first number as backend speed.
- The attached image is the only non-boot, non-system disk (`disk:1`); the
  script finds it with `Get-Disk` and mounts its GPT partitions with
  `winuxfs mount Y: disk:1 --type xfs --partition N`. Windows leaves the
  unknown (Linux file system) partitions alone. A `Process` object from
  `Start-Process -PassThru` reports `ExitCode` as null unless its `Handle` is
  read once.
- `fsutil fsinfo volumeinfo Y:` prints `Is ReadOnly` for a read-only volume
  (and a harmless `Error 1: Incorrect function` line). Write attempts fail with
  `Access denied` (open for write) or `The media is write protected`
  (mkdir, delete, rename, attribute changes).
- Symlinks are listed as plain files with size = target length; opening one for
  reading fails in .NET ("Handle does not support synchronous operations").
  `Get-FileHash` and `[IO.File]` work for every other file, including names
  mapped through `namemap`. Names longer than MAX_PATH are hashed through
  `\\?\` streams in the script.
- A hang under multi-threaded writes was the SRWLOCK recursive-read deadlock
  described in `xfs-backend.md`.
- The XFS stage takes about 70-90 s, of which about 27 s is hashing the 4 GiB
  mostly-hole file.
- Do not `pkill -f` a pattern that also matches your own shell command line.

### Manual Explorer check

1. `tools/vmctl.py up`, then open <http://127.0.0.1:8006> and log in to the
   desktop.
2. Copy `winuxfs.exe` and `winuxfs_backend_memfs.dll` into one folder in the VM
   (`tools/vmctl.py push` them, then copy from `\\host.lan\Data\...`).
3. In a terminal in the VM, run
   `winuxfs mount Y: none --type memfs --plugin-dir .` and leave it open.
4. In Explorer, open `Y:`; create a folder and a text file, rename both,
   delete them. Press Ctrl-C in the terminal (or run `winuxfs unmount Y:`) to
   unmount.
5. `tools/vmctl.py down`. To discard changes, `tools/vmctl.py restore clean`.
