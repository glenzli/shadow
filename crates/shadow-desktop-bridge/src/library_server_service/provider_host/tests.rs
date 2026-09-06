use std::{fs, io::Cursor};

use image::{DynamicImage, ImageFormat, Rgb, RgbImage};
use shadow_core::DecodeInspector;

use super::{LibraryServerPreviewInspector, PreviewRoute};

#[test]
fn public_only_runtime_does_not_claim_a_private_provider() {
    let inspector = LibraryServerPreviewInspector::public_only().expect("build public inspector");
    assert_eq!(
        inspector.provider_id(),
        "shadow-remote-library-preview-router"
    );
    assert!(inspector.provider_version().contains(";public="));
    assert!(inspector.provider_version().contains(";raster="));
    assert!(!inspector.provider_version().contains(";provider-host="));
}

#[test]
fn public_only_runtime_advertises_direct_raster_formats() {
    let inspector = LibraryServerPreviewInspector::public_only().expect("build public inspector");
    let extensions = inspector.supported_original_raster_extensions();
    assert!(
        extensions
            .iter()
            .any(|extension| extension.eq_ignore_ascii_case("jpg"))
    );
    assert!(
        extensions
            .iter()
            .any(|extension| extension.eq_ignore_ascii_case("jpeg"))
    );
}

#[test]
fn public_only_runtime_routes_supported_raster_through_direct_inspection() {
    let root = tempfile::tempdir().expect("create raster fixture root");
    let path = root.path().join("fixture.jpg");
    let image = RgbImage::from_pixel(3, 2, Rgb([12, 34, 56]));
    let mut bytes = Vec::new();
    DynamicImage::ImageRgb8(image)
        .write_to(&mut Cursor::new(&mut bytes), ImageFormat::Jpeg)
        .expect("encode raster fixture");
    fs::write(&path, bytes).expect("write raster fixture");
    let mut inspector =
        LibraryServerPreviewInspector::public_only().expect("build public inspector");

    let snapshot = inspector.inspect(&path).expect("inspect raster fixture");

    assert_eq!(snapshot.provider.id, inspector.provider_id());
    assert_eq!(snapshot.metadata.image_dimensions.width, 3);
    assert_eq!(snapshot.metadata.image_dimensions.height, 2);
    assert_eq!(
        inspector.active_route(&path).expect("read selected route"),
        PreviewRoute::Raster
    );
}
