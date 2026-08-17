//! Source-router provider selection, format support, and RAW-plan negotiation.

use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

use shadow_domain::PreviewCodec;

use crate::{
    BasicEditParameters, BridgeError, OpticsSettings, PhotoEditDetailSession,
    PhotoEditPreviewSession, RawDevelopmentPlan, RawDevelopmentQuality, RawPipelinePath,
    extract_best_photo_preview, inspect_libraw, inspect_photo,
    negotiate_photo_raw_development_plan, photo_provider_version,
    photo_raw_development_capabilities, photo_supported_raster_extensions,
    query_photo_optics_profiles, raw_development_plan_identity, render_photo_reference_proxy,
};

#[test]
#[ignore = "requires SHADOW_TEST_PRIVATE_HE_RAW and SHADOW_TEST_DECODE_HELPER"]
fn isolated_private_raw_frame_reenters_the_host_preview_bridge_from_metadata() {
    let source = PathBuf::from(
        std::env::var_os("SHADOW_TEST_PRIVATE_HE_RAW")
            .expect("SHADOW_TEST_PRIVATE_HE_RAW must identify a locally decodable HE/HE* RAW"),
    );
    let helper = PathBuf::from(
        std::env::var_os("SHADOW_TEST_DECODE_HELPER")
            .expect("SHADOW_TEST_DECODE_HELPER must identify Shadow's isolated decode helper"),
    );
    let snapshot = inspect_photo(&source).expect("inspect the private RAW in the host bridge");
    assert!(snapshot.capabilities.raw_frame.is_available());

    let staging_root = std::env::temp_dir().join(format!(
        "shadow-bridge-staged-private-raw-contract-{}",
        std::process::id()
    ));
    fs::create_dir(&staging_root).expect("create test-private staging root");
    let manifest = staging_root.join("frame.shadowrawi");
    let result = Command::new(&helper)
        .arg("raw-frame-staging")
        .arg(&source)
        .arg(&manifest)
        .arg("01234567-89ab-cdef-0123-456789abcdef")
        .output()
        .expect("launch isolated decode helper for private RawFrame staging");
    assert!(
        result.status.success(),
        "private RawFrame staging failed: {}",
        String::from_utf8_lossy(&result.stderr)
    );

    let preview = PhotoEditPreviewSession::open_with_staged_raw_development_plan_from_metadata(
        &snapshot.metadata,
        &manifest,
        1_024,
        RawDevelopmentPlan::preview(),
        &OpticsSettings::default(),
    )
    .expect("reenter the host preview bridge with the isolated private RawFrame");
    assert_eq!(
        preview.raw_pipeline_receipt().path,
        RawPipelinePath::ShadowRawFrame,
        "the desktop-facing bridge must develop the staged CFA data rather than fall back to provider RGB"
    );
    let rendered = preview
        .render(BasicEditParameters::default(), 82)
        .expect("render a host-owned staged private RawFrame preview");
    assert!(!rendered.bytes.is_empty());

    fs::remove_dir_all(staging_root).expect("remove test-private staging root");
}

#[test]
#[ignore = "requires SHADOW_TEST_PRIVATE_HE_RAW and a configured local private decoder provider"]
fn private_he_raw_provider_takes_over_after_public_browse_only_probe() {
    let path = PathBuf::from(
        std::env::var_os("SHADOW_TEST_PRIVATE_HE_RAW")
            .expect("SHADOW_TEST_PRIVATE_HE_RAW must identify a locally decodable HE/HE* RAW"),
    );
    let public = inspect_libraw(&path).expect("inspect HE/HE* through public LibRaw");
    assert!(public.capabilities.metadata.is_available());
    assert!(public.capabilities.embedded_previews.is_available());
    assert!(
        !public.capabilities.reference_rgb.is_available(),
        "fixture must require the local private provider rather than public LibRaw development"
    );

    let routed = inspect_photo(&path).expect("open HE/HE* through Shadow photo router");
    assert_eq!(routed.provider.id, "shadow-photo-router");
    assert!(routed.capabilities.metadata.is_available());
    assert!(
        routed.capabilities.reference_rgb.is_available(),
        "private provider must restore an editable reference-RGB source"
    );
    assert!(
        routed.capabilities.raw_frame.is_available(),
        "private provider must expose its validated CFA frame to Shadow's host RAW developer"
    );
    let high_quality_plan = RawDevelopmentPlan {
        quality: RawDevelopmentQuality::High,
        ..RawDevelopmentPlan::preview()
    };
    let high_quality_negotiation = negotiate_photo_raw_development_plan(&path, high_quality_plan)
        .expect("negotiate the host RAW plan through the public bridge");
    assert!(
        high_quality_negotiation.accepted()
            && high_quality_negotiation.effective == high_quality_plan,
        "the public bridge must advertise host RawFrame capabilities rather than provider RGB fallback limits"
    );

    let proxy = render_photo_reference_proxy(&path, 1_024, 82)
        .expect("render a bounded HE/HE* preview through the routed private provider");
    assert_eq!(proxy.codec, PreviewCodec::Jpeg);
    assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
    assert!(!proxy.bytes.is_empty());

    let preview = PhotoEditPreviewSession::open(&path, 1_024)
        .expect("prepare a bounded HE/HE* edit preview through Shadow's RAW developer");
    assert!(preview.raw_development_receipt().recorded());
    assert_eq!(
        preview.raw_pipeline_receipt().path,
        RawPipelinePath::ShadowRawFrame,
        "private CFA must enter the same host RAW developer as public RawFrame sources"
    );
    assert!(
        preview.sensor_clipping_mask().available
            && preview.sensor_clipping_mask().dimensions == preview.dimensions(),
        "the prepared private HE/HE* preview carries the same-pass source clipping diagnostic"
    );

    let high_quality =
        PhotoEditPreviewSession::open_with_raw_development_plan(&path, 1_024, high_quality_plan)
            .expect(
                "host RawFrame development must not be limited by provider RGB plan capabilities",
            );
    assert_eq!(
        high_quality.raw_pipeline_receipt().path,
        RawPipelinePath::ShadowRawFrame
    );
    assert_eq!(
        high_quality.raw_pipeline_receipt().effective_plan.quality,
        RawDevelopmentQuality::High
    );
}

#[test]
#[ignore = "requires SHADOW_TEST_UNSUPPORTED_RAW to point at a RAW with no editable reference RGB"]
fn unsupported_raw_edit_sessions_fail_without_entering_preparation() {
    let path =
        std::env::var_os("SHADOW_TEST_UNSUPPORTED_RAW").expect("SHADOW_TEST_UNSUPPORTED_RAW");
    let path = Path::new(&path);
    let snapshot = inspect_photo(path).expect("inspect unsupported RAW through photo router");
    assert!(
        !snapshot.capabilities.reference_rgb.is_available(),
        "fixture must not advertise editable reference RGB"
    );
    assert!(
        snapshot.capabilities.embedded_previews.is_available(),
        "fixture must retain a browseable embedded preview"
    );

    let Err(preview_error) = PhotoEditPreviewSession::open(path, 1_024) else {
        panic!("preview session must reject an unsupported RAW before preparation");
    };
    let Err(detail_error) = PhotoEditDetailSession::open(path) else {
        panic!("detail session must reject an unsupported RAW before preparation");
    };
    for error in [preview_error, detail_error] {
        assert!(
            matches!(error, BridgeError::RawDevelopmentUnavailable(message)
                if message.contains("embedded preview")),
            "unexpected unsupported-RAW error: {error}"
        );
    }
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
fn real_dng_photo_router_entries_cross_the_bridge() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));

    let snapshot = inspect_photo(&path).expect("inspect local DNG through photo router");
    assert_eq!(snapshot.provider.id, "shadow-photo-router");
    assert_eq!(snapshot.provider.version, photo_provider_version());
    assert!(snapshot.capabilities.metadata.is_available());
    assert!(snapshot.metadata.raw_dimensions.pixel_count() > 0);

    let capabilities = photo_raw_development_capabilities(&path)
        .expect("query local DNG RAW-development capabilities through router");
    assert!(capabilities.available);
    assert_eq!(
        capabilities.schema_version,
        RawDevelopmentPlan::CURRENT_SCHEMA_VERSION
    );
    let preview_negotiation =
        negotiate_photo_raw_development_plan(&path, RawDevelopmentPlan::preview())
            .expect("negotiate preview RAW-development plan");
    assert!(preview_negotiation.accepted());
    assert_eq!(preview_negotiation.effective, RawDevelopmentPlan::preview());
    let high_quality = negotiate_photo_raw_development_plan(
        &path,
        RawDevelopmentPlan {
            quality: RawDevelopmentQuality::High,
            ..RawDevelopmentPlan::preview()
        },
    )
    .expect("negotiate high-quality RAW-development plan");
    let advertises_high_quality = capabilities.supported_qualities & (1 << 2) != 0;
    assert!(high_quality.accepted());
    if advertises_high_quality {
        assert_eq!(high_quality.effective.quality, RawDevelopmentQuality::High);
    } else {
        assert!(!high_quality.exact());
        assert_ne!(high_quality.effective.quality, RawDevelopmentQuality::High);
    }

    let _profiles = query_photo_optics_profiles(&path)
        .expect("query local DNG optical profiles through router");
    let _preview = extract_best_photo_preview(&path)
        .expect("extract local DNG embedded preview through router");

    let proxy = render_photo_reference_proxy(&path, 1_024, 82)
        .expect("render local DNG reference proxy through router");
    assert_eq!(proxy.codec, PreviewCodec::Jpeg);
    assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
    assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
    assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));

    let preview = PhotoEditPreviewSession::open(&path, 1_024)
        .expect("prepare generic local DNG preview session");
    assert!(preview.raw_development_receipt().recorded());
    assert!(preview.raw_pipeline_receipt().recorded());
    assert!(!preview.raw_pipeline_receipt().cache_identity.is_empty());
    assert_eq!(
        preview.raw_development_receipt().requested_plan,
        RawDevelopmentPlan::preview()
    );
    assert_eq!(
        preview.raw_pipeline_receipt().requested_plan,
        RawDevelopmentPlan::preview()
    );
    assert_eq!(
        preview.raw_development_receipt().effective_plan_identity,
        raw_development_plan_identity(RawDevelopmentPlan::preview())
            .expect("canonical preview plan identity")
    );
    let detail =
        PhotoEditDetailSession::open(&path).expect("prepare generic local DNG detail session");
    assert!(detail.raw_development_receipt().recorded());
    assert!(detail.raw_pipeline_receipt().recorded());
    assert!(!detail.raw_pipeline_receipt().cache_identity.is_empty());
    assert_eq!(
        detail.raw_development_receipt().requested_plan,
        RawDevelopmentPlan::detail()
    );
    assert_eq!(
        detail.raw_pipeline_receipt().requested_plan,
        RawDevelopmentPlan::detail()
    );
}

#[test]
#[ignore = "requires SHADOW_TEST_JPEG to point at a local JPEG fixture"]
fn real_jpeg_photo_router_entries_cross_the_bridge() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_JPEG").expect("SHADOW_TEST_JPEG"));

    let snapshot = inspect_photo(&path).expect("inspect local JPEG through photo router");
    assert_eq!(snapshot.provider.id, "shadow-photo-router");
    assert_eq!(snapshot.provider.version, photo_provider_version());
    assert!(snapshot.capabilities.metadata.is_available());
    assert!(!snapshot.capabilities.raw_frame.is_available());
    assert!(snapshot.capabilities.reference_rgb.is_available());
    assert!(snapshot.metadata.image_dimensions.pixel_count() > 0);

    assert!(
        query_photo_optics_profiles(&path)
            .expect("query local JPEG optical profiles through router")
            .is_empty()
    );
    assert!(
        extract_best_photo_preview(&path)
            .expect("extract local JPEG preview through router")
            .is_none()
    );

    let proxy = render_photo_reference_proxy(&path, 1_024, 82)
        .expect("render local JPEG reference proxy through router");
    assert_eq!(proxy.codec, PreviewCodec::Jpeg);
    assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
    assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
    assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));

    let preview = PhotoEditPreviewSession::open(&path, 1_024)
        .expect("prepare generic local JPEG preview session");
    assert!(!preview.raw_development_receipt().recorded());
    assert_eq!(
        preview.raw_pipeline_receipt().path,
        RawPipelinePath::DecodedRaster
    );
    assert!(preview.raw_pipeline_receipt().recorded());
    let edited = preview
        .render(BasicEditParameters::default(), 82)
        .expect("render neutral generic JPEG preview session");
    assert_eq!(edited.codec, PreviewCodec::Jpeg);
    let detail =
        PhotoEditDetailSession::open(&path).expect("prepare generic local JPEG detail session");
    assert!(!detail.raw_development_receipt().recorded());
    assert_eq!(
        detail.raw_pipeline_receipt().path,
        RawPipelinePath::DecodedRaster
    );
}

#[test]
#[ignore = "requires SHADOW_TEST_HEIF to point at a local 8-bit SDR HEIF/HEIC fixture"]
fn real_heif_photo_router_entries_cross_the_bridge() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_HEIF").expect("SHADOW_TEST_HEIF"));

    let extensions = photo_supported_raster_extensions();
    assert!(extensions.contains(&"heic".to_owned()));
    assert!(extensions.contains(&"heif".to_owned()));

    let snapshot = inspect_photo(&path).expect("inspect local HEIF through photo router");
    assert_eq!(snapshot.provider.id, "shadow-photo-router");
    assert_eq!(snapshot.provider.version, photo_provider_version());
    assert!(snapshot.capabilities.metadata.is_available());
    assert!(!snapshot.capabilities.raw_frame.is_available());
    assert!(snapshot.capabilities.reference_rgb.is_available());
    assert!(snapshot.metadata.image_dimensions.pixel_count() > 0);
    assert_eq!(snapshot.metadata.orientation, 1);

    assert!(
        query_photo_optics_profiles(&path)
            .expect("query local HEIF optical profiles through router")
            .is_empty()
    );
    assert!(
        extract_best_photo_preview(&path)
            .expect("extract local HEIF preview through router")
            .is_none()
    );

    let proxy = render_photo_reference_proxy(&path, 1_024, 82)
        .expect("render local HEIF reference proxy through router");
    assert_eq!(proxy.codec, PreviewCodec::Jpeg);
    assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
    assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
    assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));

    let preview = PhotoEditPreviewSession::open(&path, 1_024)
        .expect("prepare generic local HEIF preview session");
    assert!(!preview.raw_development_receipt().recorded());
    assert_eq!(
        preview.raw_pipeline_receipt().path,
        RawPipelinePath::DecodedRaster
    );
    let edited = preview
        .render(BasicEditParameters::default(), 82)
        .expect("render neutral generic HEIF preview session");
    assert_eq!(edited.codec, PreviewCodec::Jpeg);
    let detail =
        PhotoEditDetailSession::open(&path).expect("prepare generic local HEIF detail session");
    assert!(!detail.raw_development_receipt().recorded());
    assert_eq!(
        detail.raw_pipeline_receipt().path,
        RawPipelinePath::DecodedRaster
    );
}
