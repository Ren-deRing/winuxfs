// SPDX-License-Identifier: GPL-2.0-only
//! End-to-end tests for the Windows block devices. They need the test VM
//! (docs/vm.md) with the pattern image attached as a second disk:
//!
//! ```sh
//! python3 -I tools/mkdisk.py /tmp/pattern.img
//! tools/vmctl.py attach-disk /tmp/pattern.img
//! ```
//!
//! and an elevated token (the VM's SSH account `Docker` is an administrator
//! and OpenSSH key logons get the full token). The pattern disk is found by
//! size (64 MiB) among the disks that are neither boot nor system disks and
//! not file-backed; there must be exactly one.
//!
//! All tests share one lock: they touch the same disk and the disk's
//! offline state is global.
#![cfg(windows)]

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex, MutexGuard};

use winuxfs_core::{BlockDevice, Errno};
use winuxfs_win::{attach_vhd, WinDisk};

const BLOCK: u64 = 4096;
const PATTERN_SIZE: u64 = 64 * 1024 * 1024;
const VHD_SIZE: u64 = 32 * 1024 * 1024;

static LOCK: Mutex<()> = Mutex::new(());

fn lock() -> MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

fn ps(script: &str) -> String {
    let out = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", script])
        .output()
        .expect("run powershell");
    assert!(
        out.status.success(),
        "powershell failed: {script}\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stdout).trim().to_string()
}

/// Number of the attached pattern disk (see the module docs).
fn pattern_disk() -> u32 {
    let out = ps(&format!(
        "@(Get-Disk | Where-Object {{ $_.Size -eq {PATTERN_SIZE} -and -not $_.IsBoot \
         -and -not $_.IsSystem -and $_.BusType -ne 'File Backed Virtual' }}) \
         | ForEach-Object {{ $_.Number }}"
    ));
    let nums: Vec<u32> = out.lines().map(|l| l.trim().parse().unwrap()).collect();
    assert_eq!(
        nums.len(),
        1,
        "expected exactly one 64 MiB data disk (attach tools/mkdisk.py output \
         with vmctl.py attach-disk), found {nums:?}"
    );
    nums[0]
}

/// Number of the disk holding the Windows system volume.
fn system_disk() -> u32 {
    ps("(Get-Partition -DriveLetter $env:SystemDrive[0]).DiskNumber")
        .parse()
        .unwrap()
}

fn is_offline(n: u32) -> bool {
    match ps(&format!("(Get-Disk -Number {n}).IsOffline")).as_str() {
        "True" => true,
        "False" => false,
        o => panic!("unexpected IsOffline output {o:?}"),
    }
}

fn block_number(dev: &dyn BlockDevice, block: u64) -> u64 {
    let mut b = [0u8; 8];
    dev.read_at(block * BLOCK, &mut b).unwrap();
    u64::from_le_bytes(b)
}

fn check_pattern(dev: &dyn BlockDevice) {
    let last = dev.size() / BLOCK - 1;
    for blk in [0, 100, last] {
        assert_eq!(block_number(dev, blk), blk, "block {blk}");
    }
}

#[test]
fn physical_disk_reads_pattern() {
    let _g = lock();
    let dev = WinDisk::open(pattern_disk(), true).unwrap();
    assert_eq!(dev.size(), PATTERN_SIZE);
    assert!(dev.sector_size() >= 512 && dev.sector_size().is_power_of_two());
    assert!(dev.read_only());
    check_pattern(&*dev);
    // Unaligned read inside block 1 (number 1 in the first byte).
    let mut b = [0xffu8; 3];
    dev.read_at(BLOCK - 1, &mut b).unwrap();
    assert_eq!(b, [0, 1, 0]);
    assert_eq!(dev.write_at(0, &[1]), Err(Errno::EROFS));
}

/// Writes `orig` back on drop so a failing test leaves the pattern intact.
struct Restore<'a> {
    dev: &'a dyn BlockDevice,
    off: u64,
    orig: Vec<u8>,
}

impl Drop for Restore<'_> {
    fn drop(&mut self) {
        let _ = self.dev.write_at(self.off, &self.orig);
        let _ = self.dev.flush();
    }
}

#[test]
fn physical_disk_write_roundtrip_unaligned() {
    let _g = lock();
    let dev = WinDisk::open(pattern_disk(), false).unwrap();
    assert!(!dev.read_only());
    // Bytes 0..16 of block 0 and the first bytes of block 1 at 4096.
    let mut orig = vec![0u8; 16];
    dev.read_at(0, &mut orig).unwrap();
    let restore = Restore {
        dev: &*dev,
        off: 0,
        orig: orig.clone(),
    };
    dev.write_at(1, &[0xaa, 0xbb, 0xcc]).unwrap();
    dev.flush().unwrap();
    let mut now = vec![0u8; 16];
    dev.read_at(0, &mut now).unwrap();
    let mut want = orig.clone();
    want[1..4].copy_from_slice(&[0xaa, 0xbb, 0xcc]);
    assert_eq!(now, want);
    assert_eq!(block_number(&*dev, 1), 1);
    drop(restore);
    dev.read_at(0, &mut now).unwrap();
    assert_eq!(now, orig);
    check_pattern(&*dev);
}

#[test]
fn disk_returns_online_after_drop() {
    let _g = lock();
    let n = pattern_disk();
    let before = is_offline(n);
    {
        let ro = WinDisk::open(n, true).unwrap();
        assert_eq!(
            is_offline(n),
            before,
            "read-only open must not change state"
        );
        drop(ro);
    }
    let dev = WinDisk::open(n, false).unwrap();
    assert!(is_offline(n), "write-mode open must take the disk offline");
    let dev2 = Arc::clone(&dev);
    drop(dev);
    assert!(is_offline(n), "still offline while a clone is alive");
    drop(dev2);
    assert_eq!(is_offline(n), before);
}

#[test]
fn second_writer_is_busy() {
    let _g = lock();
    let n = pattern_disk();
    let before = is_offline(n);
    let first = WinDisk::open(n, false).unwrap();
    assert!(is_offline(n));
    // A second writer is refused and leaves the offline state alone.
    assert_eq!(WinDisk::open(n, false).err(), Some(Errno::EBUSY));
    assert!(
        is_offline(n),
        "failed second open changed the offline state"
    );
    // Readers may still open the disk next to the writer.
    let reader = WinDisk::open(n, true).unwrap();
    assert_eq!(block_number(&*reader, 100), 100);
    drop(reader);
    assert!(is_offline(n));
    drop(first);
    assert_eq!(is_offline(n), before);
    // Once the first writer is gone, write mode works again.
    let again = WinDisk::open(n, false).unwrap();
    assert!(is_offline(n));
    drop(again);
    assert_eq!(is_offline(n), before);
}

#[test]
fn system_disk_write_refused() {
    let _g = lock();
    let n = system_disk();
    let res = WinDisk::open(n, false);
    assert_eq!(res.err(), Some(Errno::EBUSY));
    // Reading the system disk is allowed.
    let dev = WinDisk::open(n, true).unwrap();
    assert!(dev.size() > PATTERN_SIZE);
    let mut mbr = [0u8; 512];
    dev.read_at(0, &mut mbr).unwrap();
    assert_eq!(&mbr[510..], &[0x55, 0xaa]);
}

#[test]
fn missing_disk_is_enoent() {
    let _g = lock();
    assert_eq!(WinDisk::open(999, true).err(), Some(Errno::ENOENT));
}

/// Scratch VHDX path; removes leftovers of an earlier, killed run. An
/// attachment made by `attach_vhd` lives only as long as the process (no
/// permanent lifetime), so after a kill the file is no longer in use and
/// can be deleted here.
fn fresh_vhdx(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join("winuxfs-e2e-disk");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join(name);
    if path.exists() {
        std::fs::remove_file(&path).expect("stale VHDX still attached?");
    }
    path
}

/// Creates a fixed 32 MiB VHDX. diskpart is used instead of `New-VHD`
/// because the Hyper-V PowerShell module is not installed in the VM.
fn create_fixed_vhdx(path: &Path) {
    let script = path.with_extension("diskpart.txt");
    std::fs::write(
        &script,
        format!(
            "create vdisk file=\"{}\" maximum={} type=fixed\r\n",
            path.display(),
            VHD_SIZE / (1024 * 1024)
        ),
    )
    .unwrap();
    let out = Command::new("diskpart.exe")
        .arg("/s")
        .arg(&script)
        .output()
        .expect("run diskpart");
    let _ = std::fs::remove_file(&script);
    assert!(
        out.status.success() && path.exists(),
        "diskpart create vdisk failed: {}",
        String::from_utf8_lossy(&out.stdout)
    );
}

#[test]
fn vhdx_attach_and_read() {
    let _g = lock();
    let path = fresh_vhdx("pattern.vhdx");
    create_fixed_vhdx(&path);
    {
        let att = attach_vhd(&path, false).unwrap();
        let dev = att.device();
        assert_eq!(dev.size(), VHD_SIZE);
        assert!(!dev.read_only());
        let mut buf = vec![0u8; 1024 * 1024];
        for chunk in 0..VHD_SIZE / buf.len() as u64 {
            let first = chunk * buf.len() as u64 / BLOCK;
            for (i, b) in buf.chunks_mut(BLOCK as usize).enumerate() {
                b[..8].copy_from_slice(&(first + i as u64).to_le_bytes());
            }
            dev.write_at(chunk * buf.len() as u64, &buf).unwrap();
        }
        dev.flush().unwrap();
    }
    {
        let att = attach_vhd(&path, true).unwrap();
        let dev = att.device();
        assert!(dev.read_only());
        assert!(att.disk_number() != system_disk());
        check_pattern(&*dev);
        assert_eq!(dev.write_at(0, &[1]), Err(Errno::EROFS));
    }
    // Detached on drop: the file can be removed again.
    std::fs::remove_file(&path).expect("VHDX still attached after drop");
}

const HOLD_ENV: &str = "WINUXFS_E2E_HOLD_VHD";

/// Helper run in a child process by `vhd_detached_when_process_killed`:
/// attaches the VHDX named by `WINUXFS_E2E_HOLD_VHD`, creates
/// `<path>.ready` and waits to be killed.
#[test]
#[ignore = "helper for vhd_detached_when_process_killed"]
fn hold_vhd_attachment() {
    let path = PathBuf::from(std::env::var_os(HOLD_ENV).expect(HOLD_ENV));
    let att = attach_vhd(&path, false).unwrap();
    std::fs::write(path.with_extension("ready"), att.disk_number().to_string()).unwrap();
    std::thread::sleep(std::time::Duration::from_secs(120));
    drop(att);
}

fn vhd_disk_present(n: u32) -> bool {
    ps(&format!(
        "@(Get-Disk | Where-Object {{ $_.Number -eq {n} -and $_.BusType -eq 'File Backed Virtual' }}).Count"
    )) != "0"
}

#[test]
fn vhd_detached_when_process_killed() {
    use std::time::{Duration, Instant};
    let _g = lock();
    let path = fresh_vhdx("killed.vhdx");
    let ready = path.with_extension("ready");
    let _ = std::fs::remove_file(&ready);
    create_fixed_vhdx(&path);
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--ignored", "--exact", "hold_vhd_attachment"])
        .env(HOLD_ENV, &path)
        .spawn()
        .unwrap();
    let start = Instant::now();
    while !ready.exists() {
        assert!(
            start.elapsed() < Duration::from_secs(60),
            "child never attached"
        );
        if let Some(st) = child.try_wait().unwrap() {
            panic!("child exited early: {st}");
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let n: u32 = std::fs::read_to_string(&ready).unwrap().parse().unwrap();
    assert!(vhd_disk_present(n), "disk {n} not visible while attached");
    child.kill().unwrap(); // TerminateProcess: no destructors run
    child.wait().unwrap();
    // The kernel closes the child's handle, which detaches the disk. The
    // disk was offline (write mode); it vanishes with the detach.
    let start = Instant::now();
    while vhd_disk_present(n) {
        assert!(start.elapsed() < Duration::from_secs(30), "still attached");
        std::thread::sleep(Duration::from_millis(200));
    }
    std::fs::remove_file(&ready).unwrap();
    std::fs::remove_file(&path).expect("VHDX still in use after kill");
}

#[test]
fn vhd_missing_file_is_enoent() {
    let _g = lock();
    let path = std::env::temp_dir().join("winuxfs-no-such.vhdx");
    assert_eq!(attach_vhd(&path, true).err(), Some(Errno::ENOENT));
}
