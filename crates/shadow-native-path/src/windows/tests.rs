use super::{decode_windows_path_units, encode_windows_path_units, windows_location_from_units};
use crate::NativePathError;
use shadow_domain::Platform;

#[test]
fn ascii_units_have_an_explicit_little_endian_layout() {
    assert_eq!(
        encode_windows_path_units(&[u16::from(b'C'), u16::from(b':'), u16::from(b'\\')]),
        [b'C', 0, b':', 0, b'\\', 0]
    );
}

#[test]
fn windows_location_retains_platform_units_and_display_projection_separately() {
    let units = r"\\?\C:\照片\📷.NEF".encode_utf16().collect::<Vec<_>>();
    let location = windows_location_from_units(&units, "presentation only");

    assert_eq!(location.platform, Platform::Windows);
    assert_eq!(location.native_path, encode_windows_path_units(&units));
    assert_eq!(location.display_path, "presentation only");
}

#[test]
fn multilingual_and_emoji_paths_round_trip_as_code_units() {
    for fixture in [
        r"C:\照片\原片.NEF",
        r"C:\写真\原版.NEF",
        r"C:\photos\shadow-📷.NEF",
    ] {
        let units = fixture.encode_utf16().collect::<Vec<_>>();
        let bytes = encode_windows_path_units(&units);
        assert_eq!(
            decode_windows_path_units(&bytes).expect("decode Windows path units"),
            units
        );
    }
}

#[test]
fn unc_and_extended_length_prefixes_are_not_normalized() {
    for fixture in [
        r"\\studio-server\archive\照片.NEF",
        r"\\?\C:\archive\写真.NEF",
        r"\\?\UNC\studio-server\archive\📷.NEF",
    ] {
        let units = fixture.encode_utf16().collect::<Vec<_>>();
        let bytes = encode_windows_path_units(&units);
        assert_eq!(
            decode_windows_path_units(&bytes).expect("decode prefixed Windows path"),
            units
        );
    }
}

#[test]
fn extended_length_paths_remain_long_and_byte_exact() {
    let fixture = format!(r"\\?\C:\archive\{}.NEF", "x".repeat(300));
    let units = fixture.encode_utf16().collect::<Vec<_>>();
    assert!(units.len() > 260);

    let bytes = encode_windows_path_units(&units);
    assert_eq!(
        decode_windows_path_units(&bytes).expect("decode extended-length Windows path"),
        units
    );
}

#[test]
fn unpaired_surrogates_remain_lossless() {
    let units = [
        0xD800,
        u16::from(b'\\'),
        0xDC00,
        u16::from(b'.'),
        u16::from(b'd'),
    ];
    let bytes = encode_windows_path_units(&units);

    assert_eq!(
        decode_windows_path_units(&bytes).expect("decode unpaired surrogate units"),
        units
    );
}

#[test]
fn odd_byte_lengths_are_rejected() {
    assert_eq!(
        decode_windows_path_units(&[b'C', 0, b':']),
        Err(NativePathError::InvalidWindowsEncoding)
    );
}
