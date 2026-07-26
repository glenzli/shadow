//! Preview cancellation, routing, analysis, execution-receipt, and request contracts.

use super::*;

#[test]
fn preview_cancellation_is_shared_one_shot_state() {
    let cancellation =
        EditPreviewCancellation::new().expect("allocate native preview cancellation");
    let clone = cancellation.clone();
    assert!(
        clone.cancel(),
        "first request_stop wins across SharedPtr clones"
    );
    assert!(
        !cancellation.cancel(),
        "later cancellation requests are idempotent"
    );
}

#[test]
fn warm_edit_session_is_send_sync_and_bounded_before_raw_io() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<LibRawEditPreviewSession>();
    assert_send_sync::<PhotoEditPreviewSession>();

    assert_eq!(
        std::any::TypeId::of::<PhotoEditPreviewSession>(),
        std::any::TypeId::of::<LibRawEditPreviewSession>(),
        "the source-neutral API must not add a second session allocation or threading model"
    );

    for max_edge in [0, MAX_WARM_EDIT_PREVIEW_EDGE + 1] {
        let error = PhotoEditPreviewSession::open(
            Path::new("fixture-that-must-not-be-opened.raw"),
            max_edge,
        )
        .expect_err("invalid warm bound must fail before opening the source");
        assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
    }
}

#[test]
fn photo_router_reference_proxy_rejects_invalid_requests_before_source_io() {
    for (max_edge, jpeg_quality) in [(0, 82), (16_385, 82), (1_024, 0), (1_024, 101)] {
        let error = render_photo_reference_proxy(
            Path::new("fixture-that-must-not-be-opened.photo"),
            max_edge,
            jpeg_quality,
        )
        .expect_err("invalid generic proxy request must fail before opening the source");
        assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
    }
}

#[test]
fn photo_router_reports_mandatory_jpeg_raster_support() {
    let extensions = photo_supported_raster_extensions();
    assert!(extensions.contains(&"jpg".to_owned()));
    assert!(extensions.contains(&"jpeg".to_owned()));
}

fn valid_ffi_edit_preview_analysis() -> ffi::FfiEditPreviewAnalysis {
    let histogram = || {
        let mut bins = vec![0_u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT];
        bins[0] = 2;
        bins
    };
    ffi::FfiEditPreviewAnalysis {
        version: EDIT_PREVIEW_ANALYSIS_VERSION.to_owned(),
        sample_dimensions: ffi::FfiDimensions {
            width: 2,
            height: 1,
        },
        red: histogram(),
        green: histogram(),
        blue: histogram(),
        luma: histogram(),
        below_zero_samples: vec![0, 0, 0],
        above_one_samples: vec![1, 0, 0],
        hdr_headroom_bins: vec![1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        hdr_headroom_pixels: 1,
        hdr_peak_headroom_ev: 1.0,
        pixel_count: 2,
        shadow_clipped_pixels: 0,
        highlight_clipped_pixels: 1,
    }
}

fn valid_edit_preview_proxy() -> shadow_domain::ProxyPayload {
    shadow_domain::ProxyPayload {
        dimensions: ImageDimensions {
            width: 2,
            height: 1,
        },
        codec: PreviewCodec::Jpeg,
        bits_per_channel: 8,
        channels: 3,
        bytes: vec![0xff, 0xd8, 0xff, 0xd9],
    }
}

fn valid_ffi_edit_preview_execution_receipt() -> ffi::FfiEditPreviewExecutionReceipt {
    ffi::FfiEditPreviewExecutionReceipt {
        schema_version: EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION,
        cache_identity: concat!(
            "shadow-edit-preview-execution-v1;adjustment=cpu-v1;",
            "plan=1;display=cpu-v1;display-contract=1;route=staged"
        )
        .to_owned(),
        adjustment_backend: ffi::FfiEditPreviewBackend::Cpu,
        adjustment_backend_version: 1,
        adjustment_execution_contract_version: EDIT_PREVIEW_EXECUTION_PLAN_CONTRACT_VERSION,
        display_backend: ffi::FfiEditPreviewBackend::Cpu,
        display_backend_version: 1,
        display_output_contract_version: DISPLAY_SRGB8_OUTPUT_CONTRACT_VERSION,
        fused_pipeline: false,
        adjustment_fell_back: false,
        display_fell_back: false,
        diagnostic: String::new(),
    }
}

#[test]
fn rust_edit_preview_backend_versions_match_the_native_generator_contract() {
    let native_identity = edit_preview_generator_implementation_identity();
    assert!(
        native_identity.contains(&format!(
            "adjustment-metal=shadow-adjustment-metal-v{};",
            EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION
        )),
        "Rust's split Metal receipt version must track the native adjustment identity: \
         {native_identity}"
    );
    assert!(
        native_identity.contains("warm-fused-metal=v1;features="),
        "Rust's fused warm route must be named in the native generator identity: \
         {native_identity}"
    );

    let mut split_metal = valid_ffi_edit_preview_execution_receipt();
    split_metal.cache_identity = format!(
        "shadow-edit-preview-execution-v1;adjustment=metal-v{};\
         plan=1;display=cpu-v1;display-contract=1;route=staged",
        EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION
    );
    split_metal.adjustment_backend = ffi::FfiEditPreviewBackend::Metal;
    split_metal.adjustment_backend_version = EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION;
    edit_preview_execution_receipt(split_metal)
        .expect("the current native split Metal receipt is accepted");

    let mut stale_split_metal = valid_ffi_edit_preview_execution_receipt();
    stale_split_metal.adjustment_backend = ffi::FfiEditPreviewBackend::Metal;
    stale_split_metal.adjustment_backend_version =
        EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION - 1;
    assert!(matches!(
        edit_preview_execution_receipt(stale_split_metal),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));
}

#[test]
fn edit_preview_analysis_validation_fails_closed() {
    let proxy_dimensions = ImageDimensions {
        width: 2,
        height: 1,
    };
    let valid = validate_edit_preview_analysis(valid_ffi_edit_preview_analysis(), proxy_dimensions)
        .expect("valid analysis contract");
    assert_eq!(valid.version, EDIT_PREVIEW_ANALYSIS_VERSION);
    assert_eq!(valid.pixel_count, 2);
    assert_eq!(valid.red.iter().sum::<u64>(), 2);
    assert_eq!(valid.highlight_clipped_pixels, 1);
    assert_eq!(valid.hdr_headroom_pixels, 1);
    assert_eq!(valid.hdr_headroom_bins[0], 1);
    assert_eq!(valid.hdr_peak_headroom_ev, 1.0);

    let mut wrong_version = valid_ffi_edit_preview_analysis();
    wrong_version.version.push_str(":future");
    assert!(matches!(
        validate_edit_preview_analysis(wrong_version, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let wrong_proxy_dimensions = ImageDimensions {
        width: 1,
        height: 2,
    };
    assert!(matches!(
        validate_edit_preview_analysis(valid_ffi_edit_preview_analysis(), wrong_proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut short_histogram = valid_ffi_edit_preview_analysis();
    short_histogram.luma.pop();
    assert!(matches!(
        validate_edit_preview_analysis(short_histogram, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut wrong_histogram_sum = valid_ffi_edit_preview_analysis();
    wrong_histogram_sum.blue[0] = 1;
    assert!(matches!(
        validate_edit_preview_analysis(wrong_histogram_sum, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut malformed_hdr_headroom = valid_ffi_edit_preview_analysis();
    malformed_hdr_headroom.hdr_headroom_bins.pop();
    assert!(matches!(
        validate_edit_preview_analysis(malformed_hdr_headroom, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut impossible_hdr_peak = valid_ffi_edit_preview_analysis();
    impossible_hdr_peak.hdr_headroom_pixels = 0;
    impossible_hdr_peak.hdr_headroom_bins[0] = 0;
    assert!(matches!(
        validate_edit_preview_analysis(impossible_hdr_peak, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut impossible_channel_counts = valid_ffi_edit_preview_analysis();
    impossible_channel_counts.below_zero_samples = vec![2, 0, 0];
    impossible_channel_counts.above_one_samples = vec![1, 0, 0];
    impossible_channel_counts.shadow_clipped_pixels = 2;
    assert!(matches!(
        validate_edit_preview_analysis(impossible_channel_counts, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut impossible_any_channel_count = valid_ffi_edit_preview_analysis();
    impossible_any_channel_count.highlight_clipped_pixels = 2;
    assert!(matches!(
        validate_edit_preview_analysis(impossible_any_channel_count, proxy_dimensions),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    validate_analyzed_edit_preview(
        valid_edit_preview_proxy(),
        valid_ffi_edit_preview_analysis(),
        valid_ffi_edit_preview_execution_receipt(),
        proxy_dimensions,
    )
    .expect("matching analyzed proxy contract");

    let paired_wrong_dimensions = ImageDimensions {
        width: 1,
        height: 2,
    };
    let mut wrong_proxy = valid_edit_preview_proxy();
    wrong_proxy.dimensions = paired_wrong_dimensions;
    let mut matching_wrong_analysis = valid_ffi_edit_preview_analysis();
    matching_wrong_analysis.sample_dimensions = ffi::FfiDimensions {
        width: paired_wrong_dimensions.width,
        height: paired_wrong_dimensions.height,
    };
    assert!(matches!(
        validate_analyzed_edit_preview(
            wrong_proxy,
            matching_wrong_analysis,
            valid_ffi_edit_preview_execution_receipt(),
            proxy_dimensions,
        ),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    for mutate in [
        |proxy: &mut shadow_domain::ProxyPayload| proxy.codec = PreviewCodec::Bitmap,
        |proxy: &mut shadow_domain::ProxyPayload| proxy.bits_per_channel = 16,
        |proxy: &mut shadow_domain::ProxyPayload| proxy.channels = 4,
        |proxy: &mut shadow_domain::ProxyPayload| proxy.bytes.clear(),
    ] {
        let mut proxy = valid_edit_preview_proxy();
        mutate(&mut proxy);
        assert!(matches!(
            validate_analyzed_edit_preview(
                proxy,
                valid_ffi_edit_preview_analysis(),
                valid_ffi_edit_preview_execution_receipt(),
                proxy_dimensions,
            ),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));
    }
}

#[test]
fn edit_preview_execution_receipt_validation_is_strict_and_keeps_diagnostics_separate() {
    let mut fallback = valid_ffi_edit_preview_execution_receipt();
    fallback.adjustment_fell_back = true;
    fallback.diagnostic = "/Users/example/private/device diagnostic".to_owned();
    let validated =
        edit_preview_execution_receipt(fallback).expect("valid fallback execution receipt");
    assert_eq!(validated.adjustment_backend, EditPreviewBackend::Cpu);
    assert_eq!(
        validated.cache_identity,
        concat!(
            "shadow-edit-preview-execution-v1;adjustment=cpu-v1;",
            "plan=1;display=cpu-v1;display-contract=1;route=staged"
        )
    );
    assert_eq!(
        validated.diagnostic.as_deref(),
        Some("/Users/example/private/device diagnostic")
    );
    assert!(!validated.cache_identity.contains("/Users"));

    let mut fused = valid_ffi_edit_preview_execution_receipt();
    fused.cache_identity = format!(
        "shadow-edit-preview-execution-v1;adjustment=metal-v{};\
         plan=1;display=metal-v{};display-contract=1;route=fused",
        EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION, EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION
    );
    fused.adjustment_backend = ffi::FfiEditPreviewBackend::Metal;
    fused.adjustment_backend_version = EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION;
    fused.display_backend = ffi::FfiEditPreviewBackend::Metal;
    fused.display_backend_version = EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION;
    fused.fused_pipeline = true;
    let validated_fused =
        edit_preview_execution_receipt(fused).expect("valid fused warm Metal receipt");
    assert_eq!(
        validated_fused.adjustment_backend_version,
        EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION
    );
    assert_eq!(
        validated_fused.display_backend_version,
        EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION
    );
    assert!(validated_fused.fused_pipeline);

    let mut impossible_hybrid = valid_ffi_edit_preview_execution_receipt();
    impossible_hybrid.adjustment_backend = ffi::FfiEditPreviewBackend::Metal;
    impossible_hybrid.adjustment_backend_version = EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION;
    impossible_hybrid.fused_pipeline = true;
    assert!(matches!(
        edit_preview_execution_receipt(impossible_hybrid),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut stale = valid_ffi_edit_preview_execution_receipt();
    stale.schema_version += 1;
    assert!(matches!(
        edit_preview_execution_receipt(stale),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut missing_identity = valid_ffi_edit_preview_execution_receipt();
    missing_identity.cache_identity.clear();
    assert!(matches!(
        edit_preview_execution_receipt(missing_identity),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut stale_display = valid_ffi_edit_preview_execution_receipt();
    stale_display.display_output_contract_version += 1;
    assert!(matches!(
        edit_preview_execution_receipt(stale_display),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut stale_adjustment_backend = valid_ffi_edit_preview_execution_receipt();
    stale_adjustment_backend.adjustment_backend_version += 1;
    assert!(matches!(
        edit_preview_execution_receipt(stale_adjustment_backend),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut stale_display_backend = valid_ffi_edit_preview_execution_receipt();
    stale_display_backend.display_backend_version += 1;
    assert!(matches!(
        edit_preview_execution_receipt(stale_display_backend),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut impossible_adjustment_fallback = valid_ffi_edit_preview_execution_receipt();
    impossible_adjustment_fallback.adjustment_backend = ffi::FfiEditPreviewBackend::Metal;
    impossible_adjustment_fallback.adjustment_fell_back = true;
    impossible_adjustment_fallback.diagnostic = "impossible".to_owned();
    assert!(matches!(
        edit_preview_execution_receipt(impossible_adjustment_fallback),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut impossible_display_fallback = valid_ffi_edit_preview_execution_receipt();
    impossible_display_fallback.display_backend = ffi::FfiEditPreviewBackend::Metal;
    impossible_display_fallback.display_fell_back = true;
    impossible_display_fallback.diagnostic = "impossible".to_owned();
    assert!(matches!(
        edit_preview_execution_receipt(impossible_display_fallback),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut missing_fallback_diagnostic = valid_ffi_edit_preview_execution_receipt();
    missing_fallback_diagnostic.adjustment_fell_back = true;
    assert!(matches!(
        edit_preview_execution_receipt(missing_fallback_diagnostic),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));

    let mut stray_diagnostic = valid_ffi_edit_preview_execution_receipt();
    stray_diagnostic.diagnostic = "stray".to_owned();
    assert!(matches!(
        edit_preview_execution_receipt(stray_diagnostic),
        Err(BridgeError::InvalidEditPreviewOutput(_))
    ));
}

#[test]
fn edited_proxy_parameters_fail_closed_before_raw_io() {
    let invalid_requests = [
        EditedProxyRequest {
            edits: BasicEditParameters {
                exposure_stops: f64::NAN,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            edits: BasicEditParameters {
                contrast_factor: -0.01,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            edits: BasicEditParameters {
                white_balance_temperature: 2.0,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            edits: BasicEditParameters {
                saturation_factor: f64::INFINITY,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            max_edge: 0,
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            jpeg_quality: 0,
            ..EditedProxyRequest::default()
        },
    ];

    for request in invalid_requests {
        let error =
            render_libraw_edited_proxy(Path::new("fixture-that-must-not-be-opened.raw"), request)
                .expect_err("invalid request must fail");
        assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
    }
}
