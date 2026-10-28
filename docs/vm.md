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
`gh` is installed; rename the result to `winfsp.msi`.)

The install can be watched in a browser at <http://127.0.0.1:8006>.

## vmctl.py subcommands

| Command | What it does |
|---|---|
| `up [--timeout S]` | Creates `vm/.ssh/id_ed25519` (once) and copies its public key to `vm/oem/authorized_keys`, starts the container via `vm/run.sh` if it is not running, then waits until SSH answers (default 5400 s). |
| `down` | `podman stop -t 120`: dockur turns this into an ACPI shutdown and waits for Windows. The container is `--rm`, so it is removed afterwards. |
| `kill` | `podman kill` (crash injection). |
| `status` | Prints container and SSH state; exit 0 only when SSH is up. |
| `run -- <cmd...>` | Runs the command in the VM over SSH; stdout/stderr and the exit code pass through. sshd's shell is `cmd.exe`. Several arguments are joined with spaces (arguments containing spaces are double-quoted); a single argument is sent verbatim, so `run -- 'dir "C:\Program Files"'` works. |
| `push <local> <remote>` | Copies a file or directory to `vm/shared/<remote>` and prints its UNC path in the VM (`\\host.lan\Data\<remote>`). |
| `attach-disk <image>` | `down`, copy the image to `vm/storage2/data2.img`, `up`. |
| `snapshot <name>` | `down`, then copy `vm/storage/` to `vm/snapshots/<name>/` (VM stays stopped). |
| `restore <name>` | `kill`, then replace `vm/storage/` with `vm/snapshots/<name>/` (VM stays stopped; run `up`). |

## Layout

| Host path | Container path | Notes |
|---|---|---|
| `vm/storage/` | `/storage` | Primary disk `data.img` (64G sparse raw), UEFI vars, dockur state. Gitignored. |
| `vm/storage2/` | `/storage2` | Second disk `data2.img`. Mounted only when `data2.img` exists. Gitignored. |
| `vm/oem/` | `/oem` | `install.bat` (tracked), `authorized_keys`, `winfsp.msi`, `openssh.msi` (gitignored). |
| `vm/shared/` | `/shared` | Samba share. Gitignored. |
| `vm/snapshots/` | - | `vmctl.py snapshot` output. Gitignored. |
| `vm/.ssh/` | - | The rig's dedicated SSH key pair. Gitignored. |
| `~/다운로드/Windows11_Client_x64_en-us_26300_9457.iso` | `/custom.iso` (ro) | Local install media; override with `WINUXFS_ISO`. |

`vm/run.sh` environment overrides: `WINUXFS_VM_NAME` (default `winuxfs-vm`),
`WINUXFS_VM_IMAGE` (default `docker.io/dockurr/windows:6.06`), `WINUXFS_ISO`,
`WINUXFS_CPU` (default 4), `WINUXFS_RAM` (default `4G`).

## Facts from the dockur docs and source (image 6.06, checked 2026-10-28)

Sources: the README, `docs/environment.md`, `src/install.sh`, `src/samba.sh`
of dockur/windows and `src/disk.sh`, `src/network.sh` of qemus/qemu (the base
image).

**Image.** `docker.io/dockurr/windows` (also `ghcr.io/dockur/windows`). The rig
pins tag `6.06`. Required options: `--device=/dev/kvm --device=/dev/net/tun
--cap-add NET_ADMIN`, ports 8006 (web viewer), and a 120 s stop timeout.

**CPU / RAM.** `CPU_CORES` (default 2) and `RAM_SIZE` (default `4G`). The rig
uses 4 cores and 4G.

**Account.** dockur creates a local administrator `Docker` with password
`admin` (`USERNAME`/`PASSWORD` env). `vm/run.sh` sets both explicitly. SSH logs
in as `Docker` with key auth; since it is an administrator, OpenSSH reads the
key from `C:\ProgramData\ssh\administrators_authorized_keys`, not from the
user profile.

**Custom ISO.** Bind-mounting an ISO file at `/custom.iso` skips the download;
`VERSION` is then ignored (the source also accepts `/boot.iso`). dockur
detects the edition from the ISO (`Detected: Windows 11 Pro`, answer file
`win11x64.xml`) and boots the ISO with a small overlay `setup.img` instead of
rewriting it, so no 9 GB copy appears in `vm/storage`. `VERSION=11` is kept in
`run.sh` only as the fallback when the ISO is absent.

**SELinux and the user's ISO.** The rig's own directories are mounted with
`:Z`. The ISO lives in the user's `~/다운로드` and must not be relabeled, so it
is mounted `:ro` without `:Z`, and the container runs with
`--security-opt label=disable` so that it can read a `user_home_t` file. The
ISO keeps its `unconfined_u:object_r:user_home_t:s0` label (verified after
start). Podman still relabels the `:Z` directories (they show
`container_file_t`), which keeps the rig working if `label=disable` is ever
removed.

**OEM folder.** The `/oem` folder is copied to `C:\OEM` and `install.bat` in it
runs once, during the final step of the automatic installation (first logon).
It does not run again on later boots; to re-provision, delete `vm/storage/`
(or restore a snapshot taken before provisioning). dockur lints `install.bat`
at container start and prints warnings in `podman logs`. Our `install.bat`
logs to `C:\OEM\install.log` and copies the log to `vm/shared/oem-install.log`
when the share is reachable.

**OpenSSH.** `install.bat` uses the built-in `OpenSSH.Server` capability if it
is already installed, otherwise the bundled Win32-OpenSSH MSI
(`vm/oem/openssh.msi`). The MSI is preferred over `Add-WindowsCapability`
because the latter downloads from Windows Update, which is slow and may not
offer features-on-demand for Insider builds such as 26300.

**Shared folder.** dockur runs Samba in the container and exports `/shared` as
share `Data` on host name `host.lan`: in the VM it is `\\host.lan\Data`, also
mapped as drive `Z:` and a `Shared` desktop shortcut for the logged-on user.
`Z:` is not visible in SSH sessions (verified), so over SSH use the UNC path.
(Plan Task 12 mentions `dir Z:\`; there `Z:` means a drive winuxfs mounts, and
it should pick a free letter other than the desktop's share mapping; the
adapter tests use `Y:`.) A drive letter that a non-service process mounts
through WinFsp exists only in that process's logon session, and every SSH
connection is a new logon session, so `vmctl.py run -- dir Y:\` from a second
connection is not expected to see a mount held by another (WinFsp defines
the letter with `DefineDosDevice`; not verified separately); `tests/e2e_read.rs` runs
`cmd /c dir Y:\` from inside the test process instead.

**Second disk.** `DISK2_SIZE` + a `/storage2` mount makes dockur create
`/storage2/data2.img` (raw; `.qcow2` with `DISK_FMT=qcow2`). If
`/storage2/data2.img` already exists it is attached as-is (never shrunk; only
grown if `DISK2_SIZE` is set larger). If `/storage2` is mounted without an
image, dockur creates a blank one of `DISK2_SIZE` or, if unset, `DISK_SIZE`
(64G), so `run.sh` mounts `/storage2` only when `data2.img` exists. An image
can also be bind-mounted directly at `/data2.img` (or `/disk2.img`); the rig
copies instead, as the plan specifies, so the guest cannot modify the caller's
original file. Disks use `DISK_TYPE=scsi` by default.

**Rootless podman networking.** Under rootless podman the default NAT mode
fails silently and dockur falls back to user-mode networking via passt
(`podman logs` shows `Mode: User (passt)`). In user mode only ports in
`USER_PORTS` are forwarded to Windows, and for Windows the default is RDP
only, so `run.sh` sets `USER_PORTS=22`. Host ports are bound to 127.0.0.1
only: SSH `127.0.0.1:2222`, web viewer `127.0.0.1:8006`.

**btrfs.** dockur warns that `/storage` on btrfs "might introduce issues with
Windows Setup" and recommends disabling COW (`chattr +C`) on disk images.
On this host the install worked without that (see below). Snapshots use
`cp --reflink=auto --sparse=always`, which is near-instant on btrfs.

**WinFsp 2.x service names.** There is no service called `WinFsp`, so
`sc query WinFsp` fails with error 1060. WinFsp 2.x installs side by side
(SxS): the kernel driver service is named `WinFsp+<SxS id>`, for example
`WinFsp+20261027T233951Z`, and is `STOPPED` (exit code 1077, never started)
until the first file system loads it on demand. The user-mode launcher service
is `WinFsp.Launcher` and runs after install. To check the install:

```sh
tools/vmctl.py run -- sc query WinFsp.Launcher            # RUNNING
tools/vmctl.py run -- 'sc query type= driver state= all | findstr /i WinFsp'
tools/vmctl.py run -- '"C:\Program Files (x86)\WinFsp\bin\fsptool-x64.exe" ver'
```

The install directory is `C:\Program Files (x86)\WinFsp\` (registry
`HKLM\SOFTWARE\WOW6432Node\WinFsp\InstallDir`). `winfsp-x64.dll` is in its
`bin\` directory and is not on `PATH`: test executables that link it must
have the DLL copied next to them, or they exit with `0xC0000135`
(STATUS_DLL_NOT_FOUND) and no output.

## Verified on this host (2026-10-28)

Host: Fedora, kernel 7.2.11, podman 5.8.9 rootless, SELinux enforcing, btrfs.
Image `dockurr/windows:6.06` (QEMU 11.1.1). ISO
`Windows11_Client_x64_en-us_26300_9457.iso`: build 26300.9457, an Insider
build, detected by dockur as Windows 11 Pro.

- First `vmctl.py up` (from the local ISO, no download): about 9 minutes to SSH.
  Later boots: about 15 s to SSH.
- `run -- ver` prints `Microsoft Windows [Version 10.0.26300.9457]`.
- `install.bat`: the `OpenSSH.Server` capability was not preinstalled, so the
  bundled MSI was used (exit 0); WinFsp MSI exit 0. `sshd` RUNNING,
  `WinFsp.Launcher` RUNNING, driver `WinFsp+20261027T233951Z` installed;
  `sc start` on it gives RUNNING (the driver loads on this build).
- WinFsp works on 26300: `winfsp-tests-x64.exe` (v2.1 test release)
  `create_test getfileinfo_test rdwr_noncached_test querydir_test delete_test
  rename_test` all OK.
- `run` propagates the exit code (`run -- exit 7` gives 7).
- `push` + `\\host.lan\Data\...` is readable over SSH. `Z:` does not exist in
  the SSH session ("The system cannot find the path specified"); use the UNC
  path.
- `snapshot clean`: about 30 s including the graceful shutdown; 12 GB on disk
  but reflinked. `restore clean`: about 2 s.
- `attach-disk` with a 64 MiB file: Windows shows disk 1, 67108864 bytes,
  `IsOffline False`, partition style RAW.
- dockur's networking fell back to `Mode: User (passt)`; SSH via
  `USER_PORTS=22` works.

## Disk and VHD tests (crates/winuxfs-win/tests/e2e_disk.rs)

```sh
python3 -I tools/mkdisk.py /tmp/pattern.img        # 64 MiB, block n starts with n (u64 LE)
tools/vmctl.py attach-disk /tmp/pattern.img        # copies it to vm/storage2/data2.img
cargo test -p winuxfs-win --target x86_64-pc-windows-gnu --no-run
tools/vmctl.py push target/x86_64-pc-windows-gnu/debug/deps/e2e_disk-<hash>.exe t14/e2e_disk.exe
tools/vmctl.py run -- 'copy /y \\host.lan\Data\t14\e2e_disk.exe C:\t14\e2e_disk.exe'
tools/vmctl.py run -- 'C:\t14\e2e_disk.exe'
```

Verified 2026-10-28 (build 26300):

- **Elevation.** Opening `\\.\PhysicalDriveN` needs an administrator token.
  The SSH session of `Docker` (key auth) already has it: `whoami /groups`
  shows `Mandatory Label\High Mandatory Level` and `net session` succeeds.
  No `runas` or scheduled task is needed.
- **Disk numbers.** Disk 0 is the 64 GB system disk (`IsBoot`/`IsSystem`
  True, GPT). The attached image is disk 1 (67108864 bytes, RAW, online,
  `BusType SAS`). The tests find it by size among the non-boot, non-system,
  non-file-backed disks and require exactly one match; `WinDisk::open`
  refuses write mode on the disk holding the Windows volume (`EBUSY`).
- **Offline state.** Write-mode `WinDisk::open` sets the disk offline
  *non-persistently* and restores it when the device is dropped. If a test
  process is killed in between, the disk stays offline until the next
  reboot (`vmctl.py down`/`up`) or
  `powershell -Command "Set-Disk -Number 1 -IsOffline $false"`.
- **VHDX.** The Hyper-V PowerShell module (`New-VHD`) is not installed, so
  the tests create fixed VHDX files with `diskpart` (`create vdisk ...
  type=fixed`) under `%TEMP%\winuxfs-e2e-disk`. Attachments are tied to the
  virtual-disk handle (no `PERMANENT_LIFETIME`), so a killed test process
  detaches automatically (`vhd_detached_when_process_killed` checks this);
  each test deletes leftover files of an earlier run before it starts.
  `ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST` cannot be used: the disk is then
  not surfaced, and `GetVirtualDiskPhysicalPath` fails with error 55.
