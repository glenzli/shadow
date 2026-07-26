//! Catalog source identity, RAW isolation, optics lookup, and edit admission contracts.

use super::*;

#[test]
fn edit_service_accepts_an_original_raster_source_when_no_raw_exists() {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-raster-edit-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create raster edit fixture");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open raster edit session");
    let source_path = root
        .join("input.jpg")
        .to_str()
        .expect("raster source path")
        .to_owned();
    let registered = session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                source_path.as_bytes().to_vec(),
                source_path.clone(),
            ),
            byte_len: 2_048,
            modified_at_ms: Some(123),
            now_ms: 100,
        })
        .expect("register raster edit source");

    let state = session
        .photo_edit_state(&registered.photo_id.to_string(), &source_path)
        .expect("resolve raster edit source through generic router path");
    assert_eq!(state.photo_id, registered.photo_id.to_string());
    assert_eq!(state.source_path, source_path);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove raster edit fixture");
}

#[test]
fn optics_profile_discovery_uses_catalog_metadata_without_opening_raw_pixels() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let photo_id_parsed = photo_id.parse().expect("parse fixture photo id");
    let source = session
        .catalog
        .review_source(photo_id_parsed)
        .expect("read fixture source")
        .expect("fixture source");
    session
        .catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id: source.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 4_096,
                modified_at_ms: Some(123),
            },
            snapshot: DecoderSnapshot {
                provider: DecodeProviderSnapshot {
                    id: "metadata-only-test".into(),
                    version: "1".into(),
                    dng_sdk: false,
                    rawspeed: false,
                    jpeg: false,
                },
                metadata: RawMetadataSnapshot {
                    make: "Nikon".into(),
                    model: "Z 9".into(),
                    normalized_make: "Nikon".into(),
                    normalized_model: "Z 9".into(),
                    dng_version: None,
                    raw_count: 1,
                    raw_dimensions: ImageDimensions {
                        width: 8_256,
                        height: 5_504,
                    },
                    image_dimensions: ImageDimensions {
                        width: 8_256,
                        height: 5_504,
                    },
                    margins: ImageMargins::default(),
                    orientation: 0,
                    cfa_pattern: "RGGB".into(),
                    sensor_colors: 3,
                    sensor_bits: 14,
                    black_level: 0,
                    white_level: 16_383,
                    as_shot_neutral: [1.0; 4],
                    baseline_exposure: 0.0,
                    iso_speed: 64.0,
                    exposure_time_seconds: 1.0 / 2_000.0,
                    aperture_f_number: 6.3,
                    focal_length_mm: 300.0,
                    captured_at_unix_seconds: 1_660_000_000,
                    lens_make: "Nikon".into(),
                    lens_model: "NIKKOR Z 100-400mm f/4.5-5.6 VR S".into(),
                    focal_length_35mm: 300.0,
                },
                capabilities: DecodeCapabilitySnapshot {
                    metadata: DecodeSupport::Available,
                    embedded_previews: DecodeSupport::Available,
                    raw_frame: DecodeSupport::Unavailable,
                    reference_rgb: DecodeSupport::Unavailable,
                    pending_corrections: PendingCorrectionsSnapshot::default(),
                    raw_development: Default::default(),
                },
                previews: Vec::new(),
            },
            inspected_at_ms: 456,
        })
        .expect("record metadata-only snapshot");

    // The fixture deliberately has no file at source_path. Success proves profile discovery
    // consumed persisted EXIF rather than requiring the unsupported RAW pixel stream.
    session
        .optics_profile_candidates(&photo_id, &source_path)
        .expect("query optical profiles from Catalog EXIF");

    drop(session);
    std::fs::remove_dir_all(root).expect("remove optics metadata fixture");
}

#[test]
fn missing_catalog_optics_route_isolates_helper_enabled_raw_but_not_rasters() {
    let helper = Path::new("/helpers/shadow-decode-helper");
    assert_eq!(
        missing_catalog_optics_route(Path::new("/photos/unsafe.NEF"), Some(helper)),
        MissingCatalogOpticsRoute::IsolatedMetadata(helper),
    );
    assert_eq!(
        missing_catalog_optics_route(Path::new("/photos/already-rendered.JPG"), Some(helper)),
        MissingCatalogOpticsRoute::DirectNative,
    );
    assert_eq!(
        missing_catalog_optics_route(Path::new("/photos/ordinary.dng"), None),
        MissingCatalogOpticsRoute::DirectNative,
    );
}

#[cfg(unix)]
#[test]
fn missing_catalog_raw_optics_stops_after_an_isolated_helper_crash() {
    use std::os::unix::fs::PermissionsExt;

    let root = std::env::temp_dir().join(format!(
        "shadow-optics-metadata-crash-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create optics crash fixture");
    let source = root.join("unsafe.nef");
    // A non-image fixture makes any accidental native fallback immediately invalid. The
    // helper must crash first and the facade must return that metadata failure instead of
    // attempting the desktop-process router.
    std::fs::write(&source, b"intentionally invalid raw fixture")
        .expect("write optics crash source fixture");
    let helper = root.join("crashing-helper");
    std::fs::write(&helper, "#!/bin/sh\nkill -SEGV $$\n").expect("write crashing helper fixture");
    std::fs::set_permissions(&helper, std::fs::Permissions::from_mode(0o755))
        .expect("make crashing helper executable");

    let error = query_missing_catalog_optics_profiles(&source, &root, Some(&helper))
        .expect_err("a child crash must not fall back to native RAW optics discovery");
    assert!(
        error
            .chain()
            .any(|cause| cause.to_string().contains("isolated RAW metadata snapshot")),
        "the facade must return the isolated metadata failure before any native source open"
    );

    std::fs::remove_dir_all(root).expect("remove optics crash fixture");
}

#[test]
fn edit_service_rejects_a_path_from_another_photo() {
    let (root, session, photo_id, source_path) = test_edit_session();

    let error = session
        .photo_edit_state(&photo_id, &format!("{source_path}.other"))
        .expect_err("mismatched source must fail");

    assert!(error.to_string().contains("does not belong to photo"));
    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn full_detail_rejects_a_source_that_no_longer_matches_catalog_before_decode() {
    let (root, session, photo_id, source_path) = test_edit_session();
    std::fs::write(&source_path, b"changed after Catalog registration")
        .expect("write changed detail source");
    let request = ffi::FfiEditDetailViewportRequest {
        base_commit_id: String::new(),
        settings: ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
        render_token: session.begin_basic_edit_detail(),
        center_x: 0.5,
        center_y: 0.5,
        viewport_width: 512,
        viewport_height: 512,
        tile_side: 512,
        use_working_recipe: true,
    };

    let error = session
        .render_basic_edit_detail_viewport(&photo_id, &source_path, &request)
        .expect_err("changed source must fail before source-router decode");
    assert!(
        error
            .to_string()
            .contains("source changed since Catalog registration")
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove changed-source fixture");
}
