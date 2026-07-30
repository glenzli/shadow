//! RAW development plans, receipts, pipeline provenance, and cache identity contracts.

use shadow_domain::{
    ImageDimensions, RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN, RawTemperatureTint,
    RawWhiteBalance,
};

use crate::{
    BridgeError, DngOpcodeExecutionStatus, DngOpcodePolicy, RawCameraProfileStatus,
    RawDevelopmentIntent, RawDevelopmentPlan, RawDevelopmentPlanNegotiationStatus,
    RawDevelopmentQuality, RawDevelopmentReceipt, RawHighlightRecoveryIntent,
    RawNoiseReductionIntent, RawPipelinePath, RawPipelineReceipt, ffi,
    provider::raw_development_plan_identity,
    raw_development::{raw_development_receipt, raw_pipeline_receipt},
};

fn ffi_detail_raw_development_plan() -> ffi::FfiRawDevelopmentPlan {
    ffi::FfiRawDevelopmentPlan {
        schema_version: RawDevelopmentPlan::CURRENT_SCHEMA_VERSION,
        intent: ffi::FfiRawDevelopmentIntent::Detail,
        quality: ffi::FfiRawDevelopmentQuality::Balanced,
        dng_opcode_policy: ffi::FfiDngOpcodePolicy::ProviderDefault,
        noise_reduction: ffi::FfiRawNoiseReductionIntent::ProviderDefault,
        highlight_recovery: ffi::FfiRawHighlightRecoveryIntent::ProviderDefault,
        white_balance_mode: ffi::FfiRawWhiteBalanceMode::AsShot,
        temperature_kelvin: RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN,
        tint: 0,
    }
}

fn recorded_ffi_raw_development_receipt() -> ffi::FfiRawDevelopmentReceipt {
    ffi::FfiRawDevelopmentReceipt {
        schema_version: RawDevelopmentReceipt::CURRENT_SCHEMA_VERSION,
        provider_id: "fixture-provider".to_owned(),
        provider_version: "fixture-provider-v1".to_owned(),
        library_version: "fixture-library-v1".to_owned(),
        development_settings_signature: "fixture-request-v1".to_owned(),
        requested_plan_identity: "shadow-raw-plan-v1;fixture=requested".to_owned(),
        effective_plan_identity: "shadow-raw-plan-v1;fixture=effective".to_owned(),
        requested_plan: ffi::FfiRawDevelopmentPlan {
            schema_version: RawDevelopmentPlan::CURRENT_SCHEMA_VERSION,
            intent: ffi::FfiRawDevelopmentIntent::Preview,
            quality: ffi::FfiRawDevelopmentQuality::Balanced,
            dng_opcode_policy: ffi::FfiDngOpcodePolicy::ProviderDefault,
            noise_reduction: ffi::FfiRawNoiseReductionIntent::ProviderDefault,
            highlight_recovery: ffi::FfiRawHighlightRecoveryIntent::ProviderDefault,
            white_balance_mode: ffi::FfiRawWhiteBalanceMode::AsShot,
            temperature_kelvin: RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN,
            tint: 0,
        },
        effective_plan: ffi_detail_raw_development_plan(),
        plan_negotiation_status: ffi::FfiRawDevelopmentPlanNegotiationStatus::Adjusted,
        processed_linear_reference_contract_version: 1,
        declared_image_dimensions: ffi::FfiDimensions {
            width: 8,
            height: 4,
        },
        rendered_dimensions: ffi::FfiDimensions {
            width: 4,
            height: 2,
        },
        orientation: 5,
        half_size: true,
        use_camera_white_balance: true,
        use_camera_matrix: true,
        use_auto_brightness: false,
        use_exposure_correction: false,
        brightness: 1.25,
        maximum_adjustment_threshold: 0.125,
        output_bits_per_channel: 16,
        demosaic_quality: 3,
        output_color: 1,
        gamma_inverse_power: 1.0,
        gamma_linear_toe_slope: 1.0,
        dng_opcode_list_1_bytes: 11,
        dng_opcode_list_2_bytes: 22,
        dng_opcode_list_3_bytes: 33,
        dng_opcode_list_1_execution: ffi::FfiDngOpcodeExecutionStatus::Applied,
        dng_opcode_list_2_execution: ffi::FfiDngOpcodeExecutionStatus::DeferredToShadow,
        dng_opcode_list_3_execution: ffi::FfiDngOpcodeExecutionStatus::SkippedForPreview,
        process_warnings: 0x1024,
    }
}

fn recorded_ffi_raw_pipeline_receipt(path: ffi::FfiRawPipelinePath) -> ffi::FfiRawPipelineReceipt {
    let is_raw_frame = matches!(path, ffi::FfiRawPipelinePath::ShadowRawFrame);
    ffi::FfiRawPipelineReceipt {
        schema_version: RawPipelineReceipt::CURRENT_SCHEMA_VERSION,
        path,
        cache_identity: "raw-pipeline-receipt-v1;fixture=canonical".to_owned(),
        pipeline_identity: if is_raw_frame {
            "shadow-raw-frame-developer-v1"
        } else {
            "shadow-provider-processed-compatibility-v1"
        }
        .to_owned(),
        source_provider_id: "fixture-provider".to_owned(),
        source_provider_version: "fixture-provider-v1".to_owned(),
        fallback_reason: String::new(),
        raw_frame_schema_version: u32::from(is_raw_frame),
        raw_developer_version: u32::from(is_raw_frame),
        requested_plan: ffi_detail_raw_development_plan(),
        effective_plan: ffi_detail_raw_development_plan(),
        camera_profile_status: if is_raw_frame {
            ffi::FfiRawCameraProfileStatus::NoMatch
        } else {
            ffi::FfiRawCameraProfileStatus::NotConsidered
        },
        camera_profile_catalog_identity: if is_raw_frame {
            "dcp-catalog-v1;fixture=empty".to_owned()
        } else {
            String::new()
        },
        camera_profile_identity: String::new(),
        camera_profile_name: String::new(),
        camera_profile_diagnostic: String::new(),
        camera_profile_developer_version: if is_raw_frame {
            RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION
        } else {
            0
        },
    }
}

#[test]
#[allow(clippy::float_cmp)] // The bridge contract preserves native scalar bits verbatim.
#[allow(clippy::too_many_lines)]
fn raw_development_receipt_bridge_preserves_default_and_recorded_fields() {
    let default = raw_development_receipt(ffi::FfiRawDevelopmentReceipt {
        schema_version: 0,
        provider_id: String::new(),
        provider_version: String::new(),
        library_version: String::new(),
        development_settings_signature: String::new(),
        requested_plan_identity: String::new(),
        effective_plan_identity: String::new(),
        requested_plan: ffi_detail_raw_development_plan(),
        effective_plan: ffi_detail_raw_development_plan(),
        plan_negotiation_status: ffi::FfiRawDevelopmentPlanNegotiationStatus::Rejected,
        processed_linear_reference_contract_version: 0,
        declared_image_dimensions: ffi::FfiDimensions {
            width: 0,
            height: 0,
        },
        rendered_dimensions: ffi::FfiDimensions {
            width: 0,
            height: 0,
        },
        orientation: 0,
        half_size: false,
        use_camera_white_balance: false,
        use_camera_matrix: false,
        use_auto_brightness: false,
        use_exposure_correction: false,
        brightness: 0.0,
        maximum_adjustment_threshold: 0.0,
        output_bits_per_channel: 0,
        demosaic_quality: 0,
        output_color: 0,
        gamma_inverse_power: 0.0,
        gamma_linear_toe_slope: 0.0,
        dng_opcode_list_1_bytes: 0,
        dng_opcode_list_2_bytes: 0,
        dng_opcode_list_3_bytes: 0,
        dng_opcode_list_1_execution: ffi::FfiDngOpcodeExecutionStatus::NotDeclared,
        dng_opcode_list_2_execution: ffi::FfiDngOpcodeExecutionStatus::NotDeclared,
        dng_opcode_list_3_execution: ffi::FfiDngOpcodeExecutionStatus::NotDeclared,
        process_warnings: 0,
    })
    .expect("default RAW receipt bridge output is valid");
    assert_eq!(default, RawDevelopmentReceipt::default());
    assert!(!default.recorded());
    assert!(!default.uses_current_schema());

    let recorded = raw_development_receipt(recorded_ffi_raw_development_receipt())
        .expect("recorded RAW receipt bridge output is valid");
    assert_eq!(
        recorded,
        RawDevelopmentReceipt {
            schema_version: RawDevelopmentReceipt::CURRENT_SCHEMA_VERSION,
            provider_id: "fixture-provider".to_owned(),
            provider_version: "fixture-provider-v1".to_owned(),
            library_version: "fixture-library-v1".to_owned(),
            development_settings_signature: "fixture-request-v1".to_owned(),
            requested_plan_identity: "shadow-raw-plan-v1;fixture=requested".to_owned(),
            effective_plan_identity: "shadow-raw-plan-v1;fixture=effective".to_owned(),
            requested_plan: RawDevelopmentPlan {
                schema_version: RawDevelopmentPlan::CURRENT_SCHEMA_VERSION,
                intent: RawDevelopmentIntent::Preview,
                quality: RawDevelopmentQuality::Balanced,
                dng_opcode_policy: DngOpcodePolicy::ProviderDefault,
                noise_reduction: RawNoiseReductionIntent::ProviderDefault,
                highlight_recovery: RawHighlightRecoveryIntent::ProviderDefault,
                white_balance: RawWhiteBalance::AsShot,
            },
            effective_plan: RawDevelopmentPlan::detail(),
            plan_negotiation_status: RawDevelopmentPlanNegotiationStatus::Adjusted,
            processed_linear_reference_contract_version: 1,
            declared_image_dimensions: ImageDimensions {
                width: 8,
                height: 4,
            },
            rendered_dimensions: ImageDimensions {
                width: 4,
                height: 2,
            },
            orientation: 5,
            half_size: true,
            use_camera_white_balance: true,
            use_camera_matrix: true,
            use_auto_brightness: false,
            use_exposure_correction: false,
            brightness: 1.25,
            maximum_adjustment_threshold: 0.125,
            output_bits_per_channel: 16,
            demosaic_quality: 3,
            output_color: 1,
            gamma_inverse_power: 1.0,
            gamma_linear_toe_slope: 1.0,
            declared_dng_opcode_list_bytes: [11, 22, 33],
            dng_opcode_execution: [
                DngOpcodeExecutionStatus::Applied,
                DngOpcodeExecutionStatus::DeferredToShadow,
                DngOpcodeExecutionStatus::SkippedForPreview,
            ],
            process_warnings: 0x1024,
        }
    );
    assert!(recorded.recorded());
    assert!(recorded.uses_current_schema());

    let serialized = serde_json::to_vec(&recorded).expect("serialize receipt");
    assert_eq!(
        serde_json::from_slice::<RawDevelopmentReceipt>(&serialized).expect("deserialize receipt"),
        recorded,
        "the bridge's fixed receipt remains serializable without losing provenance"
    );

    let mut incomplete_value: serde_json::Value =
        serde_json::from_slice(&serialized).expect("decode receipt JSON value");
    let incomplete = incomplete_value
        .as_object_mut()
        .expect("receipt serializes as an object");
    for field in [
        "requested_plan_identity",
        "effective_plan_identity",
        "requested_plan",
        "effective_plan",
        "plan_negotiation_status",
        "dng_opcode_execution",
    ] {
        incomplete.remove(field);
    }
    let incomplete: RawDevelopmentReceipt = serde_json::from_value(incomplete_value)
        .expect("an incomplete v1 receipt preserves explicit unknown-plan absence");
    assert_eq!(incomplete.schema_version, 1);
    assert!(incomplete.requested_plan_identity.is_empty());
    assert_eq!(incomplete.requested_plan, RawDevelopmentPlan::detail());
    assert_eq!(
        incomplete.plan_negotiation_status,
        RawDevelopmentPlanNegotiationStatus::Rejected
    );
    assert_eq!(
        incomplete.dng_opcode_execution,
        [DngOpcodeExecutionStatus::NotDeclared; 3]
    );
}

#[test]
fn raw_pipeline_receipt_bridge_is_typed_cache_stable_and_validated() {
    let absent = raw_pipeline_receipt(ffi::FfiRawPipelineReceipt {
        schema_version: 0,
        path: ffi::FfiRawPipelinePath::DecodedRaster,
        cache_identity: String::new(),
        pipeline_identity: String::new(),
        source_provider_id: String::new(),
        source_provider_version: String::new(),
        fallback_reason: String::new(),
        raw_frame_schema_version: 0,
        raw_developer_version: 0,
        requested_plan: ffi_detail_raw_development_plan(),
        effective_plan: ffi_detail_raw_development_plan(),
        camera_profile_status: ffi::FfiRawCameraProfileStatus::NotConsidered,
        camera_profile_catalog_identity: String::new(),
        camera_profile_identity: String::new(),
        camera_profile_name: String::new(),
        camera_profile_diagnostic: String::new(),
        camera_profile_developer_version: 0,
    })
    .expect("absent RAW pipeline receipt");
    assert_eq!(absent, RawPipelineReceipt::default());
    assert!(!absent.recorded());

    let raw_frame = raw_pipeline_receipt(recorded_ffi_raw_pipeline_receipt(
        ffi::FfiRawPipelinePath::ShadowRawFrame,
    ))
    .expect("recorded Shadow RawFrame route");
    assert_eq!(raw_frame.path, RawPipelinePath::ShadowRawFrame);
    assert_eq!(
        raw_frame.cache_identity,
        "raw-pipeline-receipt-v1;fixture=canonical"
    );
    assert_eq!(raw_frame.raw_frame_schema_version, 1);
    assert_eq!(raw_frame.raw_developer_version, 1);
    assert!(!raw_frame.used_fallback());
    assert_eq!(raw_frame.requested_plan, RawDevelopmentPlan::detail());
    assert_eq!(raw_frame.effective_plan, RawDevelopmentPlan::detail());
    assert!(raw_frame.uses_current_schema());

    let serialized = serde_json::to_vec(&raw_frame).expect("serialize RAW pipeline receipt");
    assert_eq!(
        serde_json::from_slice::<RawPipelineReceipt>(&serialized)
            .expect("deserialize RAW pipeline receipt"),
        raw_frame
    );

    let mut compatibility =
        recorded_ffi_raw_pipeline_receipt(ffi::FfiRawPipelinePath::ProviderProcessedCompatibility);
    compatibility.fallback_reason = "fixture RawFrame layout is unsupported".to_owned();
    let compatibility = raw_pipeline_receipt(compatibility).expect("provider compatibility route");
    assert_eq!(
        compatibility.path,
        RawPipelinePath::ProviderProcessedCompatibility
    );
    assert_eq!(
        compatibility.fallback_reason.as_deref(),
        Some("fixture RawFrame layout is unsupported")
    );
    assert!(compatibility.used_fallback());

    let mut invalid = recorded_ffi_raw_pipeline_receipt(ffi::FfiRawPipelinePath::DecodedRaster);
    invalid.raw_frame_schema_version = 1;
    assert!(matches!(
        raw_pipeline_receipt(invalid),
        Err(BridgeError::InvalidRawPipelineReceipt(_))
    ));
}

#[test]
fn raw_pipeline_camera_profile_provenance_is_typed_and_fail_closed() {
    let mut no_match = recorded_ffi_raw_pipeline_receipt(ffi::FfiRawPipelinePath::ShadowRawFrame);
    no_match.camera_profile_status = ffi::FfiRawCameraProfileStatus::NoMatch;
    no_match.camera_profile_catalog_identity = "dcp-catalog-v1;fixture=empty".to_owned();
    no_match.camera_profile_developer_version =
        RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION;
    let no_match = raw_pipeline_receipt(no_match).expect("valid no-match provenance");
    assert_eq!(
        no_match.camera_profile_status,
        RawCameraProfileStatus::NoMatch
    );
    assert_eq!(
        no_match.camera_profile_catalog_identity,
        "dcp-catalog-v1;fixture=empty"
    );

    let mut applied = recorded_ffi_raw_pipeline_receipt(ffi::FfiRawPipelinePath::ShadowRawFrame);
    applied.camera_profile_status = ffi::FfiRawCameraProfileStatus::Applied;
    applied.camera_profile_catalog_identity = "dcp-catalog-v1;fixture=one".to_owned();
    applied.camera_profile_identity = "dcp-profile-v1;fixture=camera".to_owned();
    applied.camera_profile_name = "Fixture Camera Standard".to_owned();
    applied.camera_profile_developer_version =
        RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION;
    let applied = raw_pipeline_receipt(applied).expect("valid applied provenance");
    assert_eq!(
        applied.camera_profile_status,
        RawCameraProfileStatus::Applied
    );
    assert_eq!(applied.camera_profile_name, "Fixture Camera Standard");
    assert!(applied.camera_profile_diagnostic.is_none());

    let mut not_applied =
        recorded_ffi_raw_pipeline_receipt(ffi::FfiRawPipelinePath::ShadowRawFrame);
    not_applied.camera_profile_status = ffi::FfiRawCameraProfileStatus::MatchedNotApplied;
    not_applied.camera_profile_catalog_identity = "dcp-catalog-v1;fixture=one".to_owned();
    not_applied.camera_profile_identity = "dcp-profile-v1;fixture=camera".to_owned();
    not_applied.camera_profile_name = "Fixture Camera Standard".to_owned();
    not_applied.camera_profile_diagnostic = "profile transform was singular".to_owned();
    not_applied.camera_profile_developer_version =
        RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION;
    let not_applied =
        raw_pipeline_receipt(not_applied).expect("valid matched-not-applied provenance");
    assert_eq!(
        not_applied.camera_profile_status,
        RawCameraProfileStatus::MatchedNotApplied
    );
    assert_eq!(
        not_applied.camera_profile_diagnostic.as_deref(),
        Some("profile transform was singular")
    );

    let mut invalid = recorded_ffi_raw_pipeline_receipt(ffi::FfiRawPipelinePath::ShadowRawFrame);
    invalid.camera_profile_status = ffi::FfiRawCameraProfileStatus::Applied;
    invalid.camera_profile_developer_version =
        RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION;
    assert!(matches!(
        raw_pipeline_receipt(invalid),
        Err(BridgeError::InvalidRawPipelineReceipt(_))
    ));
}

#[test]
fn raw_development_plan_identities_are_native_canonical_and_intent_specific() {
    let preview = raw_development_plan_identity(RawDevelopmentPlan::preview())
        .expect("native preview plan identity");
    let detail = raw_development_plan_identity(RawDevelopmentPlan::detail())
        .expect("native detail plan identity");
    let export = raw_development_plan_identity(RawDevelopmentPlan::export_image())
        .expect("native export plan identity");
    assert_eq!(
        preview,
        raw_development_plan_identity(RawDevelopmentPlan::preview())
            .expect("repeat native preview plan identity")
    );
    assert_ne!(preview, detail);
    assert_ne!(detail, export);
    assert!(preview.starts_with("shadow-raw-plan-v1;"));
    assert!(preview.contains(";wb=as-shot"));

    let value = RawTemperatureTint::new(4_800, 17).expect("manual white balance");
    let manual =
        RawDevelopmentPlan::preview().with_white_balance(RawWhiteBalance::temperature_tint(value));
    let manual_identity =
        raw_development_plan_identity(manual).expect("manual RAW white-balance identity");
    assert!(manual_identity.contains(";wb=temperature-tint:4800:17"));
    assert_ne!(manual_identity, preview);

    let invalid = RawDevelopmentPlan {
        schema_version: RawDevelopmentPlan::CURRENT_SCHEMA_VERSION + 1,
        ..RawDevelopmentPlan::preview()
    };
    assert!(matches!(
        raw_development_plan_identity(invalid),
        Err(BridgeError::InvalidRawDevelopmentPlan(_))
    ));
}

#[test]
fn raw_white_balance_bridge_round_trips_and_rejects_noncanonical_payloads() {
    let value = RawTemperatureTint::new(6_200, -8).expect("manual white balance");
    let manual =
        RawDevelopmentPlan::detail().with_white_balance(RawWhiteBalance::temperature_tint(value));
    let ffi_manual = crate::raw_development::ffi_raw_development_plan(manual);
    assert!(matches!(
        ffi_manual.white_balance_mode,
        ffi::FfiRawWhiteBalanceMode::TemperatureTint
    ));
    assert_eq!(ffi_manual.temperature_kelvin, 6_200);
    assert_eq!(ffi_manual.tint, -8);

    let receipt = raw_development_receipt(ffi::FfiRawDevelopmentReceipt {
        requested_plan: ffi_manual,
        effective_plan: ffi_manual,
        ..recorded_ffi_raw_development_receipt()
    })
    .expect("manual RAW white balance receipt");
    assert_eq!(receipt.requested_plan, manual);
    assert_eq!(receipt.effective_plan, manual);

    let mut noncanonical_as_shot = ffi_detail_raw_development_plan();
    noncanonical_as_shot.temperature_kelvin = 6_200;
    let invalid = raw_development_receipt(ffi::FfiRawDevelopmentReceipt {
        requested_plan: noncanonical_as_shot,
        ..recorded_ffi_raw_development_receipt()
    });
    assert!(matches!(
        invalid,
        Err(BridgeError::InvalidRawDevelopmentPlan(_))
    ));
}
