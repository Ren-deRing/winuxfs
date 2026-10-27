// SPDX-License-Identifier: GPL-2.0-only
//! Device specifications as given on the command line. Pure parsing, so it
//! compiles and is tested on every platform.

use std::path::PathBuf;

/// What a device argument names.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DeviceSpec {
    /// `\\.\PhysicalDriveN` or `disk:N`.
    PhysicalDrive(u32),
    /// A file with extension `.vhd` or `.vhdx` (any case).
    Vhd(PathBuf),
    /// Anything else: a raw image file.
    Image(PathBuf),
}

/// Classifies a device argument. Never fails: a string that is not a valid
/// physical drive spec (for example `disk:abc`) is treated as a file path.
pub fn parse_device_spec(s: &str) -> DeviceSpec {
    let number = strip_prefix_ci(s, r"\\.\PhysicalDrive").or_else(|| s.strip_prefix("disk:"));
    if let Some(n) = number.and_then(parse_decimal) {
        return DeviceSpec::PhysicalDrive(n);
    }
    // Both separators count on every host, so the result does not depend on
    // where the parser runs.
    let name = s.rsplit(['/', '\\']).next().unwrap_or(s);
    let ext = match name.rfind('.') {
        Some(i) if i > 0 => &name[i + 1..],
        _ => "",
    };
    if ext.eq_ignore_ascii_case("vhd") || ext.eq_ignore_ascii_case("vhdx") {
        DeviceSpec::Vhd(PathBuf::from(s))
    } else {
        DeviceSpec::Image(PathBuf::from(s))
    }
}

fn strip_prefix_ci<'a>(s: &'a str, prefix: &str) -> Option<&'a str> {
    let head = s.get(..prefix.len())?;
    head.eq_ignore_ascii_case(prefix)
        .then(|| &s[prefix.len()..])
}

/// Plain ASCII decimal digits that fit in u32 (no sign, no spaces).
fn parse_decimal(s: &str) -> Option<u32> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn img(s: &str) -> DeviceSpec {
        DeviceSpec::Image(PathBuf::from(s))
    }

    #[test]
    fn device_spec_parsing() {
        assert_eq!(
            parse_device_spec(r"\\.\PhysicalDrive3"),
            DeviceSpec::PhysicalDrive(3)
        );
        assert_eq!(parse_device_spec("disk:3"), DeviceSpec::PhysicalDrive(3));
        assert_eq!(
            parse_device_spec("a.vhdx"),
            DeviceSpec::Vhd(PathBuf::from("a.vhdx"))
        );
        assert_eq!(parse_device_spec("x.img"), img("x.img"));
        assert_eq!(parse_device_spec("disk:abc"), img("disk:abc"));
    }

    #[test]
    fn device_spec_edge_cases() {
        assert_eq!(
            parse_device_spec(r"\\.\physicaldrive0"),
            DeviceSpec::PhysicalDrive(0)
        );
        assert_eq!(
            parse_device_spec(r"C:\vm\B.VHD"),
            DeviceSpec::Vhd(PathBuf::from(r"C:\vm\B.VHD"))
        );
        // Not plain decimal digits, or out of u32 range: a file path.
        for s in [
            "disk:",
            "disk:+3",
            "disk:-1",
            "disk: 3",
            "disk:4294967296",
            r"\\.\PhysicalDrive",
            r"\\.\PhysicalDrive3x",
        ] {
            assert_eq!(parse_device_spec(s), img(s), "{s}");
        }
        assert_eq!(
            parse_device_spec("disk:4294967295"),
            DeviceSpec::PhysicalDrive(u32::MAX)
        );
        // A .vhd suffix that is not an extension, and no extension at all.
        assert_eq!(parse_device_spec("vhd"), img("vhd"));
        assert_eq!(parse_device_spec("disk.vhdx.img"), img("disk.vhdx.img"));
    }
}
