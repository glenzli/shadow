//! Concurrent photo delivery, grid-proxy identity, and native source admission contracts.

use std::path::Path;

use shadow_bridge::{RawDevelopmentPlan, photo_provider_version, raw_development_plan_identity};
use shadow_core::DecodeInspector;

use crate::{
    DesktopSession,
    isolated_proxy::NativeDecodeAdmission,
    photo_provider::{PHOTO_GRID_PROXY_JPEG_QUALITY, PHOTO_GRID_PROXY_MAX_EDGE, PhotoInspector},
    session_photo_source::reject_quarantined_native_decode,
};

#[test]
fn desktop_session_can_back_concurrent_qt_image_requests() {
    fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<DesktopSession>();
}

#[test]
fn generated_photo_proxy_cache_identity_matches_its_encoder_request() {
    let inspector = PhotoInspector::new().expect("construct photo inspector");

    assert_eq!(inspector.provider_id(), "shadow-photo-router");
    assert_eq!(inspector.provider_version(), photo_provider_version());
    assert_eq!(PHOTO_GRID_PROXY_MAX_EDGE, 2_048);
    assert_eq!(PHOTO_GRID_PROXY_JPEG_QUALITY, 90);
    let raw_plan_identity = raw_development_plan_identity(RawDevelopmentPlan::preview())
        .expect("canonical preview plan identity");
    let proxy_variant_key = inspector.proxy_variant_key();
    assert!(
        proxy_variant_key.starts_with("shadow-photo-router:grid-jpeg-2048-q90-444-v3;raw-plan-b3=")
    );
    assert!(proxy_variant_key.len() <= 256);
    assert!(!proxy_variant_key.contains(&raw_plan_identity));
    let extensions = inspector.supported_original_raster_extensions();
    assert!(extensions.contains(&"jpg".to_owned()));
    assert!(extensions.contains(&"jpeg".to_owned()));
}

#[test]
fn quarantined_native_decode_is_rejected_before_source_opening() {
    let source = Path::new("/photos/unsafe.raw");
    let error = reject_quarantined_native_decode(
        NativeDecodeAdmission::Quarantined {
            observation: crate::isolated_proxy::IsolatedDecodeObservation::ChildCrashed,
        },
        source,
    )
    .expect_err("a child decoder crash must block the direct native opener");
    assert!(error.to_string().contains("preview-only"));
    assert!(error.to_string().contains("child decoder crashed"));
    assert!(
        reject_quarantined_native_decode(NativeDecodeAdmission::NotQuarantined, source).is_ok()
    );
}
