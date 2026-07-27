//! Opt-in local RAW, DNG, JPEG, and HEIF end-to-end bridge contracts.

use super::*;
use shadow_domain::DecodeSupport;

#[test]
#[ignore = "requires SHADOW_TEST_RAW_FOLDER to contain local RAW fixtures"]
#[allow(clippy::too_many_lines)] // Keeps the end-to-end local fixture contract in one test.
fn real_raw_folder_smoke_matrix() {
    fn collect_raws(directory: &Path, paths: &mut Vec<std::path::PathBuf>) {
        let entries = std::fs::read_dir(directory)
            .unwrap_or_else(|error| panic!("read RAW fixture directory {directory:?}: {error}"));
        for entry in entries {
            let entry = entry.unwrap_or_else(|error| panic!("read RAW fixture entry: {error}"));
            let path = entry.path();
            if path.is_dir() {
                collect_raws(&path, paths);
                continue;
            }
            let extension = path
                .extension()
                .and_then(std::ffi::OsStr::to_str)
                .unwrap_or_default()
                .to_ascii_lowercase();
            if matches!(
                extension.as_str(),
                "3fr"
                    | "arw"
                    | "cr2"
                    | "cr3"
                    | "dng"
                    | "erf"
                    | "fff"
                    | "iiq"
                    | "kdc"
                    | "mef"
                    | "mos"
                    | "mrw"
                    | "nef"
                    | "nrw"
                    | "orf"
                    | "pef"
                    | "raf"
                    | "raw"
                    | "rw2"
                    | "rwl"
                    | "sr2"
                    | "srf"
                    | "srw"
            ) {
                paths.push(path);
            }
        }
    }

    let folder = std::env::var_os("SHADOW_TEST_RAW_FOLDER")
        .expect("SHADOW_TEST_RAW_FOLDER must identify a fixture directory");
    let folder = Path::new(&folder);
    let mut paths = Vec::new();
    collect_raws(folder, &mut paths);
    paths.sort();
    assert!(
        !paths.is_empty(),
        "RAW fixture directory contains no supported files"
    );

    let mut failures = Vec::new();
    let mut passed = 0_usize;
    let mut preview_only = 0_usize;
    let mut processed_rgb_compatibility = 0_usize;
    for path in paths {
        let result = (|| -> Result<(String, RawSmokePath), String> {
            const FULL_DECODE_UNAVAILABLE: &str = "RAW frame/reference RGB unavailable";
            let snapshot = inspect_libraw(&path).map_err(|error| format!("inspect: {error}"))?;
            if snapshot.provider.id != "libraw" {
                return Err(format!("unexpected provider {}", snapshot.provider.id));
            }
            if !snapshot.capabilities.metadata.is_available() {
                return Err("metadata unavailable".to_owned());
            }
            if snapshot.metadata.raw_dimensions.pixel_count() == 0 {
                return Err("invalid RAW dimensions".to_owned());
            }

            let preview = extract_best_libraw_preview(&path)
                .map_err(|error| format!("extract preview: {error}"))?
                .map(|preview| {
                    if preview.descriptor.dimensions.pixel_count() == 0 || preview.bytes.is_empty()
                    {
                        return Err("invalid embedded preview".to_owned());
                    }
                    Ok(preview)
                })
                .transpose()?;

            let raw_frame_available = snapshot.capabilities.raw_frame.is_available();
            let reference_rgb_available = snapshot.capabilities.reference_rgb.is_available();

            let profile_count = query_libraw_optics_profiles(&path)
                .map_err(|error| format!("query Lensfun profiles: {error}"))?
                .len();
            let summary = format!(
                "{} {} · {}x{} · {profile_count} compatible optical profiles",
                snapshot.metadata.normalized_make,
                snapshot.metadata.normalized_model,
                snapshot.metadata.raw_dimensions.width,
                snapshot.metadata.raw_dimensions.height,
            );
            if !reference_rgb_available {
                if preview.is_none() {
                    return Err(format!("{FULL_DECODE_UNAVAILABLE}; no embedded preview"));
                }
                return Ok((
                    format!("{summary} · embedded-preview fallback"),
                    RawSmokePath::EmbeddedPreview,
                ));
            }

            let proxy = render_libraw_reference_proxy(&path, 1_024, 82)
                .map_err(|error| format!("render reference proxy: {error}"))?;
            if proxy.codec != PreviewCodec::Jpeg {
                return Err(format!("unexpected proxy codec: {:?}", proxy.codec));
            }
            if proxy.dimensions.width.max(proxy.dimensions.height) > 1_024
                || proxy.dimensions.pixel_count() == 0
                || proxy.bytes.is_empty()
            {
                return Err("invalid bounded reference proxy".to_owned());
            }

            let path = if raw_frame_available {
                RawSmokePath::RawFrame
            } else {
                RawSmokePath::ProcessedRgbCompatibility
            };
            Ok((summary, path))
        })();

        match result {
            Ok((summary, path_kind)) => {
                passed += 1;
                match path_kind {
                    RawSmokePath::RawFrame => {
                        eprintln!("RAW smoke ok: {} · {summary}", path.display());
                    }
                    RawSmokePath::ProcessedRgbCompatibility => {
                        processed_rgb_compatibility += 1;
                        eprintln!(
                            "RAW smoke processed-RGB compatibility: {} · {summary}",
                            path.display()
                        );
                    }
                    RawSmokePath::EmbeddedPreview => {
                        preview_only += 1;
                        eprintln!("RAW smoke preview-only: {} · {summary}", path.display());
                    }
                }
            }
            Err(error) => {
                eprintln!("RAW smoke failed: {} · {error}", path.display());
                failures.push(format!("{} · {error}", path.display()));
            }
        }
    }

    assert!(
        failures.is_empty(),
        "RAW smoke matrix: {passed} passed ({preview_only} preview-only), {} failed:\n{}",
        failures.len(),
        failures.join("\n")
    );
    eprintln!(
        "RAW smoke matrix passed: {passed} files · {processed_rgb_compatibility} processed-RGB compatibility · {preview_only} preview-only fallbacks"
    );
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RawSmokePath {
    RawFrame,
    ProcessedRgbCompatibility,
    EmbeddedPreview,
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

    let preview_error = match PhotoEditPreviewSession::open(path, 1_024) {
        Ok(_) => panic!("preview session must reject an unsupported RAW before preparation"),
        Err(error) => error,
    };
    let detail_error = match PhotoEditDetailSession::open(path) {
        Ok(_) => panic!("detail session must reject an unsupported RAW before preparation"),
        Err(error) => error,
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

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
#[allow(clippy::too_many_lines)] // One ignored end-to-end bridge scenario spans all handles.
fn real_dng_raw_development_receipts_cross_all_prepared_handles() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
    let handle = open_libraw(&path).expect("open local DNG");
    let decoder = handle.as_ref().expect("non-null decoder handle");

    let before_preparation = raw_development_receipt(
        decoder
            .raw_development_receipt()
            .expect("read empty RAW development receipt"),
    )
    .expect("bridge empty RAW development receipt");
    assert_eq!(before_preparation, RawDevelopmentReceipt::default());
    let pipeline_before_preparation = raw_pipeline_receipt(
        decoder
            .raw_pipeline_receipt()
            .expect("read empty RAW pipeline receipt"),
    )
    .expect("bridge empty RAW pipeline receipt");
    assert_eq!(pipeline_before_preparation, RawPipelineReceipt::default());

    let preview_handle = decoder
        .prepare_edit_preview(1_024)
        .expect("prepare local DNG warm preview");
    let preview = preview_handle.as_ref().expect("non-null preview handle");
    let preview_receipt = raw_development_receipt(
        preview
            .raw_development_receipt()
            .expect("read preview RAW development receipt"),
    )
    .expect("bridge preview RAW development receipt");
    assert!(preview_receipt.recorded());
    assert!(preview_receipt.uses_current_schema());
    assert_eq!(preview_receipt.provider_id, "libraw");
    let preview_pipeline_receipt = raw_pipeline_receipt(
        preview
            .raw_pipeline_receipt()
            .expect("read preview RAW pipeline receipt"),
    )
    .expect("bridge preview RAW pipeline receipt");
    assert!(preview_pipeline_receipt.recorded());
    assert_eq!(
        preview_pipeline_receipt.requested_plan,
        RawDevelopmentPlan::preview()
    );
    assert_eq!(
        raw_pipeline_receipt(
            decoder
                .raw_pipeline_receipt()
                .expect("read decoder preview RAW pipeline receipt"),
        )
        .expect("bridge decoder preview RAW pipeline receipt"),
        preview_pipeline_receipt
    );
    assert_eq!(
        raw_development_receipt(
            decoder
                .raw_development_receipt()
                .expect("read decoder preview RAW development receipt"),
        )
        .expect("bridge decoder preview RAW development receipt"),
        preview_receipt,
        "DecodeHandle reports the last source render it prepared"
    );

    let detail_handle = decoder
        .prepare_edit_detail()
        .expect("prepare local DNG full detail");
    let detail = detail_handle.as_ref().expect("non-null detail handle");
    let detail_receipt = raw_development_receipt(
        detail
            .raw_development_receipt()
            .expect("read detail RAW development receipt"),
    )
    .expect("bridge detail RAW development receipt");
    assert!(detail_receipt.recorded());
    assert!(detail_receipt.uses_current_schema());
    assert_eq!(detail_receipt.provider_id, "libraw");
    assert!(!detail_receipt.half_size);
    let detail_pipeline_receipt = raw_pipeline_receipt(
        detail
            .raw_pipeline_receipt()
            .expect("read detail RAW pipeline receipt"),
    )
    .expect("bridge detail RAW pipeline receipt");
    assert!(detail_pipeline_receipt.recorded());
    assert_eq!(
        detail_pipeline_receipt.requested_plan,
        RawDevelopmentPlan::detail()
    );
    assert_eq!(
        raw_pipeline_receipt(
            decoder
                .raw_pipeline_receipt()
                .expect("read decoder detail RAW pipeline receipt"),
        )
        .expect("bridge decoder detail RAW pipeline receipt"),
        detail_pipeline_receipt
    );
    assert_eq!(
        raw_development_receipt(
            decoder
                .raw_development_receipt()
                .expect("read decoder detail RAW development receipt"),
        )
        .expect("bridge decoder detail RAW development receipt"),
        detail_receipt,
        "DecodeHandle updates its read-only receipt when a new source render is prepared"
    );
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG to identify a camera present in Lensfun"]
fn real_dng_enumerates_compatible_lensfun_profiles() {
    let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
    let candidates =
        query_libraw_optics_profiles(Path::new(&path)).expect("query Lensfun candidates");
    assert!(!candidates.is_empty());
    assert!(candidates.iter().all(|candidate| {
        !candidate.camera_model.is_empty() && !candidate.lens_model.is_empty()
    }));
}

#[test]
#[ignore = "requires SHADOW_TEST_METADATA_RAW to identify a camera present in Lensfun"]
fn raw_metadata_snapshot_enumerates_lensfun_profiles_without_pixel_decode() {
    let path = std::env::var_os("SHADOW_TEST_METADATA_RAW").expect("SHADOW_TEST_METADATA_RAW");
    let snapshot = inspect_photo(Path::new(&path)).expect("inspect metadata-only RAW");
    assert_eq!(snapshot.capabilities.metadata, DecodeSupport::Available);

    // Candidate enumeration receives only the detached snapshot. The source path and decode
    // handle cannot cross this boundary, regardless of the provider's pixel capabilities.
    let candidates = query_optics_profiles_from_metadata(&snapshot.metadata);
    assert!(!candidates.is_empty());
    assert!(candidates.iter().all(|candidate| {
        !candidate.camera_model.is_empty() && !candidate.lens_model.is_empty()
    }));
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

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
fn real_dng_warm_edit_session_renders_twice() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
    std::thread::Builder::new()
        .name("small-edit-worker".to_owned())
        .stack_size(512 * 1_024)
        .spawn(move || {
            let session = LibRawEditPreviewSession::open(&path, 1_024)
                .expect("prepare warm local DNG edit session on a small worker stack");
            let sensor_clipping = session.sensor_clipping_mask();
            assert!(sensor_clipping.available);
            assert_eq!(sensor_clipping.dimensions, session.dimensions());
            assert_eq!(
                sensor_clipping.samples.len() as u64,
                sensor_clipping.dimensions.pixel_count()
            );
            assert_eq!(
                sensor_clipping
                    .samples
                    .iter()
                    .filter(|sample| **sample & SensorClippingMask::HIGHLIGHT_BIT != 0)
                    .count() as u64,
                sensor_clipping.highlight_pixel_count
            );
            assert_eq!(
                sensor_clipping
                    .samples
                    .iter()
                    .filter(|sample| **sample & SensorClippingMask::SHADOW_BIT != 0)
                    .count() as u64,
                sensor_clipping.shadow_pixel_count
            );
            let neutral = session
                .render(BasicEditParameters::default(), 86)
                .expect("render neutral warm preview");
            let neutral_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                .expect("build neutral typed plan");
            let cancelled = EditPreviewCancellation::new().expect("allocate cancellation source");
            assert!(cancelled.cancel());
            assert!(matches!(
                session
                    .render_plan_cancellable(&neutral_plan, 86, &cancelled)
                    .expect("pre-cancelled warm render is control flow"),
                CancellableEditPreview::Cancelled
            ));
            assert!(matches!(
                session
                    .render_plan_with_analysis_cancellable(&neutral_plan, 86, &cancelled)
                    .expect("pre-cancelled analyzed render is control flow"),
                CancellableEditPreview::Cancelled
            ));
            let neutral_from_plan = session
                .render_plan(&neutral_plan, 86)
                .expect("render neutral typed plan");
            let neutral_analyzed = session
                .render_plan_with_analysis(&neutral_plan, 86)
                .expect("render neutral typed plan with analysis");
            let adjusted = session
                .render(
                    BasicEditParameters {
                        exposure_stops: 1.0,
                        contrast_factor: 1.1,
                        white_balance_temperature: 0.12,
                        white_balance_tint: 0.04,
                        saturation_factor: 1.15,
                    },
                    86,
                )
                .expect("render adjusted warm preview");
            let mut curved_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                .expect("build neutral typed plan");
            curved_plan.nodes.insert(
                4,
                AdjustmentRenderNode {
                    node_id: "test-tone-curve".to_owned(),
                    enabled: true,
                    parameter_schema_version: OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                    implementation_version: OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
                    operation: AdjustmentRenderOperation::OklabLightnessToneCurve {
                        curve: Box::new(OklabLightnessToneCurve {
                            lightness: vec![
                                ToneCurvePoint { x: 0.0, y: 0.0 },
                                ToneCurvePoint { x: 0.5, y: 0.7 },
                                ToneCurvePoint { x: 1.0, y: 1.0 },
                            ],
                        }),
                    },
                },
            );
            let curved = session
                .render_plan(&curved_plan, 86)
                .expect("render typed Tone Curve plan");

            assert_eq!(session.dimensions(), neutral.dimensions);
            assert_eq!(neutral_from_plan.bytes, neutral.bytes);
            assert_eq!(neutral_analyzed.proxy.bytes, neutral.bytes);
            assert_eq!(
                neutral_analyzed.analysis.sample_dimensions,
                neutral.dimensions
            );
            assert_eq!(
                neutral_analyzed.analysis.pixel_count,
                neutral.dimensions.pixel_count()
            );
            for histogram in [
                &neutral_analyzed.analysis.red,
                &neutral_analyzed.analysis.green,
                &neutral_analyzed.analysis.blue,
                &neutral_analyzed.analysis.luma,
            ] {
                assert_eq!(
                    histogram.iter().sum::<u64>(),
                    neutral_analyzed.analysis.pixel_count
                );
            }
            assert_eq!(
                neutral_analyzed.analysis.version,
                EDIT_PREVIEW_ANALYSIS_VERSION
            );
            assert_eq!(adjusted.dimensions, neutral.dimensions);
            assert_eq!(curved.dimensions, neutral.dimensions);
            assert_eq!(neutral.codec, PreviewCodec::Jpeg);
            assert!(neutral.bytes.starts_with(&[0xff, 0xd8]));
            assert!(adjusted.bytes.ends_with(&[0xff, 0xd9]));
            assert_ne!(adjusted.bytes, neutral.bytes);
            assert_ne!(curved.bytes, neutral.bytes);
        })
        .expect("spawn small edit worker")
        .join()
        .expect("small edit worker did not panic");
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
fn real_dng_full_edit_detail_session_renders_deterministic_tiles() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
    std::thread::Builder::new()
        .name("small-detail-worker".to_owned())
        .stack_size(512 * 1_024)
        .spawn(move || {
            let session = LibRawEditDetailSession::open(&path)
                .expect("prepare full local DNG detail session on a small worker stack");
            let full = session.dimensions();
            assert!(full.width > 0 && full.height > 0);
            assert!((1..=MAX_EDIT_DETAIL_RETAINED_BYTES).contains(&session.retained_bytes()));
            let width = full.width.min(512);
            let height = full.height.min(512);
            let request = DetailTileRequest {
                rect: DetailTileRect {
                    x: (full.width - width) / 2,
                    y: (full.height - height) / 2,
                    width,
                    height,
                },
            };
            let neutral_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                .expect("build neutral detail plan");
            let first = session
                .render_plan_tile(&neutral_plan, request)
                .expect("render neutral full-resolution detail tile");
            let second = session
                .render_plan_tile(&neutral_plan, request)
                .expect("repeat neutral full-resolution detail tile");
            assert_eq!(first, second);
            assert_eq!(first.rect, request.rect);
            assert_eq!(first.full_dimensions, full);
            assert_eq!(first.row_stride_bytes, width * 3);
            assert_eq!(
                first.bytes.len(),
                usize::try_from(width * 3)
                    .unwrap()
                    .checked_mul(usize::try_from(height).unwrap())
                    .unwrap()
            );

            let adjusted_plan = basic_adjustment_render_plan(BasicEditParameters {
                exposure_stops: 1.0,
                ..BasicEditParameters::default()
            })
            .expect("build adjusted detail plan");
            let adjusted = session
                .render_plan_tile(&adjusted_plan, request)
                .expect("render adjusted full-resolution detail tile");
            assert_ne!(adjusted.bytes, first.bytes);
        })
        .expect("spawn small detail worker")
        .join()
        .expect("small detail worker did not panic");
}
