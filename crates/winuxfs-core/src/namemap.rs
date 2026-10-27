//! Reversible mapping between on-disk names (arbitrary bytes) and Windows
//! UTF-16 names.
//!
//! # Scheme
//!
//! The name is split into valid UTF-8 characters and invalid bytes (using the
//! standard lossy-decoder segmentation: every byte of an invalid sequence is
//! its own "invalid byte", always >= 0x80). Each element is mapped to UTF-16:
//!
//! * invalid byte `b`                          -> `U+F700 | b`
//! * NUL, control 0x01-0x1F, `< > : " \ | ? *` -> `U+F000 | code`
//! * literal `U+F000-U+F0FF`, `U+F700-U+F7FF`, `U+F8FF` -> `U+F8FF` then the char
//! * anything else                             -> itself
//!
//! Finally, every `.` / ` ` in the trailing run of dots and spaces becomes
//! `U+F02E` / `U+F020`. (The brief's wording says "trailing '.' and space";
//! its test `"x. "` requires both of the last two units mapped, so the whole
//! run is mapped: Windows strips *any* trailing run, not just one char.
//! Inner dots/spaces are legal and kept.)
//!
//! # Injectivity
//!
//! Decoding is local: `U+F8FF X` -> X; `U+F0xx` -> byte `xx`; `U+F7xx` -> byte
//! `xx`; other chars -> themselves. Every encoder output unit (or escape pair)
//! decodes to exactly the element that produced it, because the three
//! private-use ranges are only ever emitted by the escape rules (literals in
//! those ranges are always prefixed), and the trailing rule emits `U+F02E` /
//! `U+F020`, which the `U+F0xx` rule already decodes to `.`/` ` (an escaped literal U+F02E/U+F020 is
//! prefixed with `U+F8FF`, so it never collides). Hence
//! `from_windows(to_windows(x)) == x`, so the mapping is injective. Decisions:
//! NUL follows the control-char rule (`U+F000`); `/` cannot occur in a disk
//! name and is passed through unchanged (still reversible).
//!
//! `from_windows` is strict: it decodes then re-encodes and requires the
//! result to equal the input, so every non-canonical name (raw reserved chars,
//! raw trailing dot, mid-name `U+F02E`, `U+F7xx` with xx < 0x80, dangling
//! `U+F8FF`, invalid bytes that would form valid UTF-8, ...) is `Err(EINVAL)`
//! and no two accepted inputs alias the same disk name.

use crate::Errno;

const ESC: u16 = 0xF8FF;

fn is_special(c: char) -> bool {
    matches!(c as u32, 0xF000..=0xF0FF | 0xF700..=0xF7FF | 0xF8FF)
}

fn is_reserved(c: char) -> bool {
    matches!(c, '<' | '>' | ':' | '"' | '\\' | '|' | '?' | '*') || (c as u32) < 0x20
}

fn push_char(c: char, out: &mut Vec<u16>) {
    if is_special(c) {
        out.push(ESC);
        out.push(c as u16);
    } else if is_reserved(c) {
        out.push(0xF000 | c as u16);
    } else {
        let mut buf = [0u16; 2];
        out.extend_from_slice(c.encode_utf16(&mut buf));
    }
}

/// Map an on-disk name to a Windows UTF-16 name. Never panics.
pub fn to_windows(name: &[u8]) -> Vec<u16> {
    let mut out = Vec::with_capacity(name.len());
    let mut rest = name;
    while !rest.is_empty() {
        match std::str::from_utf8(rest) {
            Ok(s) => {
                s.chars().for_each(|c| push_char(c, &mut out));
                break;
            }
            Err(e) => {
                let (good, bad) = rest.split_at(e.valid_up_to());
                // `good` is valid UTF-8 by construction.
                if let Ok(s) = std::str::from_utf8(good) {
                    s.chars().for_each(|c| push_char(c, &mut out));
                }
                let n = e.error_len().unwrap_or(bad.len()).max(1);
                for &b in &bad[..n] {
                    out.push(0xF700 | u16::from(b));
                }
                rest = &bad[n..];
            }
        }
    }
    for u in out.iter_mut().rev() {
        match *u {
            0x2E => *u = 0xF02E,
            0x20 => *u = 0xF020,
            _ => break,
        }
    }
    out
}

/// Inverse of [`to_windows`]. Lone surrogates and any input that
/// `to_windows` could not have produced yield `Err(EINVAL)`.
pub fn from_windows(name: &[u16]) -> Result<Vec<u8>, Errno> {
    let mut out = Vec::with_capacity(name.len());
    let mut escaped = false;
    for r in char::decode_utf16(name.iter().copied()) {
        let c = r.map_err(|_| Errno::EINVAL)?;
        let cp = c as u32;
        if escaped {
            escaped = false;
            let mut buf = [0u8; 4];
            out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
        } else if cp == u32::from(ESC) {
            escaped = true;
        } else if matches!(cp, 0xF000..=0xF0FF | 0xF700..=0xF7FF) {
            out.push((cp & 0xFF) as u8);
        } else {
            let mut buf = [0u8; 4];
            out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
        }
    }
    if escaped || to_windows(&out) != name {
        return Err(Errno::EINVAL);
    }
    Ok(out)
}
