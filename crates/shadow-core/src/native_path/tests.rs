use std::path::Path;

use shadow_domain::{AssetLocation, Platform};

use super::{NativePathError, current_platform, native_location, native_path_from_location};

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
fn foreign_platform_locations_are_rejected() {
    let foreign = match current_platform() {
        Platform::Windows => Platform::MacOs,
        Platform::MacOs | Platform::OtherUnix => Platform::Windows,
    };
    let location = AssetLocation::new(foreign, Vec::new(), "foreign");

    assert!(matches!(
        native_path_from_location(&location),
        Err(NativePathError::PlatformMismatch { .. })
    ));
}
