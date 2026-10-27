use proptest::prelude::*;
use winuxfs_core::namemap::{from_windows, to_windows};
use winuxfs_core::Errno;

proptest! {
    #![proptest_config(ProptestConfig::with_cases(1024))]
    #[test]
    fn roundtrip_property(v in proptest::collection::vec(any::<u8>(), 0..=255)) {
        prop_assert_eq!(from_windows(&to_windows(&v)), Ok(v));
    }

    // Biased toward tricky bytes: escapes, UTF-8 lead/continuation bytes, dots, spaces.
    #[test]
    fn roundtrip_tricky_alphabet(v in proptest::collection::vec(
        proptest::sample::select(vec![
            0x00u8, 0x01, 0x1F, b'.', b' ', b':', b'a', 0x7F, 0x80, 0xBF, 0xEE, 0xEF,
            0xF0, 0xFF, 0xE2, 0x82, 0xAC, 0xBC, 0xA0,
        ]), 0..=40)) {
        prop_assert_eq!(from_windows(&to_windows(&v)), Ok(v));
    }

    #[test]
    fn injective_on_pairs(a in proptest::collection::vec(any::<u8>(), 0..8),
                          b in proptest::collection::vec(any::<u8>(), 0..8)) {
        prop_assume!(a != b);
        prop_assert_ne!(to_windows(&a), to_windows(&b));
    }

    #[test]
    fn from_windows_never_panics(w in proptest::collection::vec(any::<u16>(), 0..64)) {
        let _ = from_windows(&w);
    }

    // Anything from_windows accepts must map back to exactly the same input.
    #[test]
    fn accepted_windows_names_are_canonical(w in proptest::collection::vec(
        proptest::sample::select(vec![
            0x61u16, 0x2E, 0x20, 0x3A, 0x00, 0xF000, 0xF020, 0xF02E, 0xF03A, 0xF73F,
            0xF780, 0xF7E2, 0xF782, 0xF7AC, 0xF8FF, 0xD800, 0x20AC,
        ]), 0..=8)) {
        if let Ok(b) = from_windows(&w) {
            prop_assert_eq!(to_windows(&b), w);
        }
    }
}

#[test]
fn reserved_chars_become_private_use() {
    assert_eq!(to_windows(b"a:b"), vec![0x61, 0xF03A, 0x62]);
}

#[test]
fn trailing_dot_and_space() {
    let w = to_windows(b"x. ");
    assert_eq!(&w[w.len() - 2..], &[0xF02E, 0xF020]);
}

#[test]
fn invalid_utf8_byte_roundtrips() {
    let v = vec![0x66, 0xFF];
    assert_eq!(to_windows(&v), vec![0x66, 0xF7FF]);
    assert_eq!(from_windows(&to_windows(&v)), Ok(v));
}

#[test]
fn literal_private_use_is_escaped() {
    let lit = "\u{F03A}";
    assert_ne!(to_windows(lit.as_bytes()), to_windows(b":"));
    assert_eq!(to_windows(lit.as_bytes()), vec![0xF8FF, 0xF03A]);
}

#[test]
fn lone_surrogate_rejected() {
    assert_eq!(from_windows(&[0xD800]), Err(Errno::EINVAL));
    assert_eq!(from_windows(&[0x61, 0xDC00, 0x62]), Err(Errno::EINVAL));
}

#[test]
fn reserved_device_names_pass_through() {
    assert_eq!(to_windows(b"con"), vec![0x63, 0x6F, 0x6E]);
}

#[test]
fn invalid_byte_next_to_multibyte() {
    for v in [
        vec![0xE2, 0x82, 0xE2, 0x82, 0xAC],
        vec![0x80, 0xE2, 0x82, 0xAC, 0x80],
        vec![0xE2, 0x82, 0xAC, 0xAC],
        vec![0xF0, 0x9F, 0x98],
        vec![0xEF, 0xA0, 0x80],
    ] {
        assert_eq!(from_windows(&to_windows(&v)), Ok(v));
    }
    // A valid sequence and the same bytes as "invalid" must not collide.
    assert_eq!(to_windows(&[0xE2, 0x82, 0xAC]), vec![0x20AC]);
}

#[test]
fn trailing_runs_and_edges() {
    assert_eq!(to_windows(b"a.."), vec![0x61, 0xF02E, 0xF02E]);
    assert_eq!(to_windows(b"a  "), vec![0x61, 0xF020, 0xF020]);
    assert_eq!(to_windows(b"."), vec![0xF02E]);
    assert_eq!(to_windows(b"a. b."), vec![0x61, 0x2E, 0x20, 0x62, 0xF02E]);
    assert_eq!(to_windows(b""), Vec::<u16>::new());
    assert_eq!(from_windows(&[]), Ok(vec![]));
    assert_ne!(to_windows("a\u{F02E}".as_bytes()), to_windows(b"a."));
    for v in [
        b"a:.".to_vec(),
        "\u{F8FF}.".as_bytes().to_vec(),
        b" ".to_vec(),
        b"a. .".to_vec(),
    ] {
        assert_eq!(from_windows(&to_windows(&v)), Ok(v));
    }
}

#[test]
fn nul_and_slash_and_long() {
    assert_eq!(to_windows(&[0x61, 0x00]), vec![0x61, 0xF000]);
    assert_ne!(to_windows(&[0]), to_windows("\u{F000}".as_bytes()));
    assert_eq!(from_windows(&to_windows(b"a/b")), Ok(b"a/b".to_vec()));
    let long = vec![0xFFu8; 255];
    assert_eq!(from_windows(&to_windows(&long)), Ok(long));
    let long = "\u{F8FF}".repeat(85).into_bytes();
    assert_eq!(long.len(), 255);
    assert_eq!(from_windows(&to_windows(&long)), Ok(long));
}

#[test]
fn non_canonical_input_rejected() {
    for w in [
        vec![0xF8FF],
        vec![0xF8FF, 0x61],
        vec![0xF02E, 0x61],
        vec![0x61, 0x2E],
        vec![0x61, 0x2E, 0xF02E],
        vec![0x61, 0xF02E, 0x2E],
        vec![0x3A],
        vec![0xF73F],
        vec![0xF761],
        vec![0x0000],
        vec![0xF7E2, 0xF782, 0xF7AC],
        vec![0xF03F, 0x61, 0xF8FF],
    ] {
        assert_eq!(from_windows(&w), Err(Errno::EINVAL), "{w:x?}");
    }
    assert_eq!(from_windows(&[0xF03F, 0x61]), Ok(b"?a".to_vec()));
}
