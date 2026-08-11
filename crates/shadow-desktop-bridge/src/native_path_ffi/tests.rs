use shadow_domain::Platform;
use shadow_native_path::{current_platform, encode_windows_path_units};

use super::{location_from_ffi, location_to_ffi, path_from_ffi};
use crate::ffi::{FfiNativePath, FfiNativePathPlatform};

#[test]
fn windows_units_cross_the_ffi_without_unicode_normalization() {
    let units = vec![b'C'.into(), b':'.into(), b'\\'.into(), 0xd800, b'x'.into()];
    let input = FfiNativePath {
        platform: FfiNativePathPlatform::Windows,
        unix_bytes: Vec::new(),
        windows_units: units.clone(),
        display_path: "display-only".into(),
    };

    let location = location_from_ffi(&input).expect("encode Windows native path");
    assert_eq!(location.platform, Platform::Windows);
    assert_eq!(location.native_path, encode_windows_path_units(&units));
    let projected = location_to_ffi(&location).expect("project Windows native path");
    assert_eq!(projected.windows_units, units);
    assert!(projected.unix_bytes.is_empty());
}

#[test]
fn unix_bytes_cross_the_ffi_without_utf8() {
    let input = FfiNativePath {
        platform: FfiNativePathPlatform::OtherUnix,
        unix_bytes: vec![b'/', b't', b'm', b'p', b'/', 0xff],
        windows_units: Vec::new(),
        display_path: "/tmp/replacement".into(),
    };

    let location = location_from_ffi(&input).expect("encode Unix native path");
    assert_eq!(location.native_path, input.unix_bytes);
    assert_eq!(location.display_path, "/tmp/replacement");
}

#[test]
fn platform_payloads_cannot_be_mixed() {
    let windows_with_unix = FfiNativePath {
        platform: FfiNativePathPlatform::Windows,
        unix_bytes: vec![b'x'],
        windows_units: vec![b'x'.into()],
        display_path: String::new(),
    };
    assert!(location_from_ffi(&windows_with_unix).is_err());

    let unix_with_windows = FfiNativePath {
        platform: FfiNativePathPlatform::OtherUnix,
        unix_bytes: vec![b'x'],
        windows_units: vec![b'x'.into()],
        display_path: String::new(),
    };
    assert!(location_from_ffi(&unix_with_windows).is_err());
}

#[test]
fn foreign_platform_is_rejected_before_display_text_is_consulted() {
    let (platform, unix_bytes, windows_units) = match current_platform() {
        Platform::Windows => (FfiNativePathPlatform::OtherUnix, vec![b'/'], Vec::new()),
        Platform::MacOs | Platform::OtherUnix => (
            FfiNativePathPlatform::Windows,
            Vec::new(),
            vec![b'C'.into(), b':'.into(), b'\\'.into()],
        ),
    };
    let input = FfiNativePath {
        platform,
        unix_bytes,
        windows_units,
        display_path: "a valid local path must not override platform identity".into(),
    };
    assert!(path_from_ffi(&input).is_err());
}
