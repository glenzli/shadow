use std::path::Path;

use shadow_domain::{AssetLocation, Platform};

use super::{current_platform, native_location, native_path_from_location};
use crate::NativePathError;

#[test]
fn native_location_round_trips_without_using_the_display_path() {
    let expected = Path::new("/photos/source");
    let mut location = native_location(expected);
    location.display_path = "not-the-native-path".into();

    assert_eq!(
        native_path_from_location(&location).expect("decode current-platform path"),
        expected
    );
}

#[test]
fn foreign_platform_locations_are_rejected_before_decoding() {
    let foreign = match current_platform() {
        Platform::Windows => Platform::MacOs,
        Platform::MacOs | Platform::OtherUnix => Platform::Windows,
    };
    let location = AssetLocation::new(foreign, [b'C', 0, b':'].to_vec(), "foreign");

    assert!(matches!(
        native_path_from_location(&location),
        Err(NativePathError::PlatformMismatch { .. })
    ));
}

#[cfg(unix)]
#[test]
fn unix_paths_preserve_non_utf8_bytes() {
    use std::{ffi::OsStr, os::unix::ffi::OsStrExt};

    let bytes = b"/photos/non-utf8-\xFF.nef";
    let path = Path::new(OsStr::from_bytes(bytes));
    let location = native_location(path);

    assert_eq!(location.native_path, bytes);
    assert_eq!(
        native_path_from_location(&location).expect("decode Unix native bytes"),
        path
    );
}
