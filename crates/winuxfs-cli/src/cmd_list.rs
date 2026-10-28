// SPDX-License-Identifier: GPL-2.0-only
//! `list-backends` and `list-partitions`; both work on every host.

use std::path::PathBuf;

use winuxfs_abi as abi;
use winuxfs_core::part::{PartKind, PartitionInfo};
use winuxfs_core::plugin::discover;

use crate::common::{guid_text, open_device, partitions, plugin_dirs, CliError};

/// Capability names, in bit order.
fn caps_text(caps: u32) -> String {
    let names = [
        (abi::WFS_CAP_READONLY, "readonly"),
        (abi::WFS_CAP_SINGLE_THREADED, "single-threaded"),
        (abi::WFS_CAP_XATTR, "xattr"),
        (abi::WFS_CAP_HARDLINK, "hardlink"),
        (abi::WFS_CAP_SYMLINK, "symlink"),
    ];
    let mut v: Vec<&str> = names
        .iter()
        .filter(|(bit, _)| caps & bit != 0)
        .map(|(_, n)| *n)
        .collect();
    if v.is_empty() {
        v.push("-");
    }
    v.join(",")
}

/// Prints one line per plugin file found: name, capabilities and path, or
/// the load error. Broken plugins are reported, not fatal.
pub fn list_backends(explicit: &[PathBuf]) -> Result<(), CliError> {
    let dirs = plugin_dirs(explicit);
    let found = discover(&dirs);
    if found.is_empty() {
        let shown: Vec<String> = dirs.iter().map(|d| d.display().to_string()).collect();
        println!(
            "no backends found (searched: {}); add plugin directories with \
             --plugin-dir or WINUXFS_PLUGIN_PATH",
            if shown.is_empty() {
                "nothing".to_string()
            } else {
                shown.join(", ")
            }
        );
        return Ok(());
    }
    for (path, r) in found {
        match r {
            Ok(b) => println!(
                "{:<12} caps={:<24} {}",
                b.name(),
                caps_text(b.caps()),
                path.display()
            ),
            Err(e) => println!("{:<12} {}: {e}", "ERROR", path.display()),
        }
    }
    Ok(())
}

fn mbr_type_name(t: u8) -> &'static str {
    match t {
        0x07 => "NTFS/exFAT",
        0x0B | 0x0C => "FAT32",
        0x0E => "FAT16",
        0x82 => "Linux swap",
        0x83 => "Linux",
        0x8E => "Linux LVM",
        0xEF => "EFI system",
        0xFD => "Linux RAID",
        _ => "",
    }
}

fn gpt_type_name(guid: &str) -> &'static str {
    match guid {
        "0FC63DAF-8483-4772-8E79-3D69D8477DE4" => "Linux filesystem",
        "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F" => "Linux swap",
        "E6D6D379-F507-44C2-A23C-238F2A3DF928" => "Linux LVM",
        "A19D880F-05FC-4D3B-A006-743F0F84911E" => "Linux RAID",
        "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" => "Microsoft basic data",
        "E3C9E316-0B5C-4DB8-817D-F92DF00215AE" => "Microsoft reserved",
        "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" => "EFI system",
        "21686148-6449-6E6F-744E-656564454649" => "BIOS boot",
        _ => "",
    }
}

fn kind_text(k: &PartKind) -> String {
    match k {
        PartKind::Mbr { type_id } => {
            let n = mbr_type_name(*type_id);
            if n.is_empty() {
                format!("mbr 0x{type_id:02x}")
            } else {
                format!("mbr 0x{type_id:02x} ({n})")
            }
        }
        PartKind::Gpt { type_guid, name } => {
            let g = guid_text(type_guid);
            let t = gpt_type_name(&g);
            let mut s = format!("gpt {g}");
            if !t.is_empty() {
                s.push_str(&format!(" ({t})"));
            }
            if !name.is_empty() {
                s.push_str(&format!(" \"{}\"", name.escape_debug()));
            }
            s
        }
    }
}

/// The partition table as text: a header line, then one row per partition
/// with number, start and length in bytes, and type.
pub fn partition_table(parts: &[PartitionInfo]) -> String {
    let mut s = format!("{:>4}  {:>16}  {:>16}  TYPE", "PART", "START", "LENGTH");
    for p in parts {
        s.push_str(&format!(
            "\n{:>4}  {:>16}  {:>16}  {}",
            p.index,
            p.start,
            p.len,
            kind_text(&p.kind)
        ));
    }
    s
}

/// Prints the partition table of `device` (opened read-only).
pub fn list_partitions(device: &str) -> Result<(), CliError> {
    let opened = open_device(device, true)?;
    let parts = partitions(device, opened.dev.as_ref())?;
    if parts.is_empty() {
        println!(
            "{device}: no partition table; the whole device ({} bytes) holds the file system",
            opened.dev.size()
        );
    } else {
        println!("{}", partition_table(&parts));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn caps_are_named() {
        assert_eq!(caps_text(0), "-");
        assert_eq!(caps_text(8 | 16), "hardlink,symlink");
    }

    #[test]
    fn table_rows() {
        let t = partition_table(&[PartitionInfo {
            index: 2,
            start: 4096,
            len: 512,
            kind: PartKind::Mbr { type_id: 0x83 },
        }]);
        let row = t.lines().nth(1).unwrap();
        let f: Vec<&str> = row.split_whitespace().collect();
        assert_eq!(f[..4], ["2", "4096", "512", "mbr"]);
        assert!(row.ends_with("0x83 (Linux)"));
    }
}
