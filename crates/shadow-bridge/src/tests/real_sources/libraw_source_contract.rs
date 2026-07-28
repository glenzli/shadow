//! Direct LibRaw inspection, embedded-preview, and proxy entry-point contracts.

use std::path::Path;

use shadow_domain::PreviewCodec;

use crate::{
    BasicEditParameters, EditedProxyRequest, RawDevelopmentPlan, extract_best_libraw_preview,
    inspect_libraw, render_libraw_edited_proxy, render_libraw_reference_proxy,
};

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
fn real_dng_snapshot_crosses_the_bridge() {
    let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
    let snapshot = inspect_libraw(Path::new(&path)).expect("inspect local DNG");
    assert_eq!(snapshot.provider.id, "libraw");
    assert!(snapshot.capabilities.metadata.is_available());
    assert!(snapshot.capabilities.raw_frame.is_available());
    assert!(
        snapshot
            .capabilities
            .raw_development
            .available
            .is_available()
    );
    assert_eq!(
        snapshot.capabilities.raw_development.plan_schema_version,
        RawDevelopmentPlan::CURRENT_SCHEMA_VERSION
    );
    assert!(snapshot.metadata.raw_dimensions.pixel_count() > 0);
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG_WITH_PREVIEW to point at a local RAW fixture"]
fn real_dng_embedded_preview_crosses_the_bridge() {
    let path =
        std::env::var_os("SHADOW_TEST_DNG_WITH_PREVIEW").expect("SHADOW_TEST_DNG_WITH_PREVIEW");
    let preview = extract_best_libraw_preview(Path::new(&path))
        .expect("extract local DNG preview")
        .expect("fixture contains a preview");
    assert_eq!(preview.descriptor.codec, PreviewCodec::Jpeg);
    assert!(preview.descriptor.dimensions.pixel_count() > 0);
    assert_eq!(
        preview.descriptor.encoded_bytes,
        u64::try_from(preview.bytes.len()).expect("preview length fits u64")
    );
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG_NO_PREVIEW to point at a local RAW fixture"]
fn real_dng_reference_proxy_crosses_the_bridge() {
    let path = std::env::var_os("SHADOW_TEST_DNG_NO_PREVIEW").expect("SHADOW_TEST_DNG_NO_PREVIEW");
    let proxy =
        render_libraw_reference_proxy(Path::new(&path), 2_048, 88).expect("render local DNG proxy");
    assert_eq!(proxy.codec, PreviewCodec::Jpeg);
    assert_eq!(proxy.dimensions.width, 2_048);
    assert_eq!(proxy.dimensions.height, 1_536);
    assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
    assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
fn real_dng_edited_proxy_crosses_the_bridge() {
    let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
    let proxy = render_libraw_edited_proxy(
        Path::new(&path),
        EditedProxyRequest {
            edits: BasicEditParameters {
                exposure_stops: 0.5,
                contrast_factor: 1.1,
                white_balance_temperature: 0.12,
                white_balance_tint: 0.04,
                saturation_factor: 1.15,
            },
            max_edge: 1_024,
            jpeg_quality: 86,
        },
    )
    .expect("render edited local DNG proxy");
    assert_eq!(proxy.codec, PreviewCodec::Jpeg);
    assert_eq!(proxy.dimensions.width.max(proxy.dimensions.height), 1_024);
    assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
    assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));
}
