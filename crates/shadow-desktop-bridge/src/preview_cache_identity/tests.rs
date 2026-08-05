use super::*;
use shadow_bridge::{
    DngOpcodePolicy, EditPreviewBackend, RawCameraProfileStatus, RawDevelopmentIntent,
    RawDevelopmentPlan, RawDevelopmentQuality, RawHighlightRecoveryIntent, RawNoiseReductionIntent,
    RawPipelinePath,
};

fn receipt(canonical_identity: &str) -> RawPipelineReceipt {
    let plan = RawDevelopmentPlan {
        schema_version: RawDevelopmentPlan::CURRENT_SCHEMA_VERSION,
        intent: RawDevelopmentIntent::Preview,
        quality: RawDevelopmentQuality::Balanced,
        dng_opcode_policy: DngOpcodePolicy::ProviderDefault,
        noise_reduction: RawNoiseReductionIntent::ProviderDefault,
        highlight_recovery: RawHighlightRecoveryIntent::ProviderDefault,
        white_balance: shadow_domain::RawWhiteBalance::AsShot,
    };
    RawPipelineReceipt {
        schema_version: RawPipelineReceipt::CURRENT_SCHEMA_VERSION,
        path: RawPipelinePath::ShadowRawFrame,
        cache_identity: canonical_identity.to_owned(),
        pipeline_identity: "fixture-pipeline".to_owned(),
        source_provider_id: "fixture-provider".to_owned(),
        source_provider_version: "1".to_owned(),
        fallback_reason: None,
        raw_frame_schema_version: 2_026_080_601,
        raw_developer_version: 1,
        requested_plan: plan,
        effective_plan: plan,
        camera_profile_status: RawCameraProfileStatus::NoMatch,
        camera_profile_catalog_identity: "fixture-catalog".to_owned(),
        camera_profile_identity: String::new(),
        camera_profile_name: String::new(),
        camera_profile_diagnostic: None,
        camera_profile_developer_version:
            RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION,
    }
}

#[test]
fn prepared_identity_is_bounded_and_hides_native_diagnostics() {
    let diagnostic = "private path /Users/example/profile.dcp; fallback details";
    let identity =
        prepared_raw_pipeline_cache_identity(&receipt(diagnostic)).expect("compact receipt");

    assert!(identity.component().starts_with("raw-pipeline-v1-"));
    assert_eq!(
        identity.component().len(),
        "raw-pipeline-v1-".len() + SHORT_DIGEST_BYTES * 2
    );
    assert!(!identity.component().contains(diagnostic));
    assert_ne!(
        identity,
        prepared_raw_pipeline_cache_identity(&receipt("different canonical receipt"))
            .expect("compact changed receipt")
    );
}

#[test]
fn absent_or_stale_receipts_cannot_enter_a_durable_cache_key() {
    assert!(prepared_raw_pipeline_cache_identity(&RawPipelineReceipt::default()).is_err());
    let mut stale = receipt("canonical");
    stale.schema_version = RawPipelineReceipt::CURRENT_SCHEMA_VERSION + 1;
    assert!(prepared_raw_pipeline_cache_identity(&stale).is_err());
}

#[test]
fn effective_backend_changes_the_bounded_durable_identity() {
    let cpu = prepared_raw_pipeline_cache_identity(&receipt(
        "raw-pipeline-receipt-v1;pipeline=shadow-raw-frame-v1;\
         backend=shadow-fused-raw-cpu-v1;sensor-highlights=neutral-v1",
    ))
    .expect("compact CPU receipt");
    let metal = prepared_raw_pipeline_cache_identity(&receipt(
        "raw-pipeline-receipt-v1;pipeline=shadow-raw-frame-v1;\
         backend=shadow-fused-raw-metal-full-v1;math=f32-precise;sensor-highlights=neutral-v1",
    ))
    .expect("compact Metal receipt");

    assert_ne!(cpu, metal);
}

#[test]
fn edit_preview_generator_is_known_before_preparing_a_source() {
    let current = edit_preview_generator_version(
        "source-environment-v1-current-environment",
        "shadow-edit-preview-generator-v1;cpu=1",
    );
    assert_eq!(
        current,
        format!(
            "shadow-edit-preview-v1;environment=source-environment-v1-current-environment;\
             implementation={}",
            short_digest(b"shadow-edit-preview-generator-v1;cpu=1")
        )
    );
    assert_ne!(
        current,
        edit_preview_generator_version(
            "source-environment-v1-other-environment",
            "shadow-edit-preview-generator-v1;cpu=1",
        )
    );
    assert_ne!(
        current,
        edit_preview_generator_version(
            "source-environment-v1-current-environment",
            "shadow-edit-preview-generator-v1;cpu=2",
        )
    );
    assert!(!current.contains("raw-pipeline"));
}

#[test]
fn edit_execution_identity_uses_only_the_native_canonical_route() {
    let canonical = "shadow-edit-preview-execution-v1;adjustment=cpu-v1;plan=1;\
         display=cpu-v1;display-contract=1;route=staged";
    let receipt = EditPreviewExecutionReceipt {
        schema_version: 1,
        cache_identity: canonical.into(),
        adjustment_backend: EditPreviewBackend::Cpu,
        adjustment_backend_version: 1,
        adjustment_execution_contract_version: 1,
        display_backend: EditPreviewBackend::Cpu,
        display_backend_version: 1,
        display_output_contract_version: 1,
        fused_pipeline: false,
        adjustment_fell_back: true,
        display_fell_back: false,
        diagnostic: Some("/Users/example/private Metal diagnostic".into()),
    };
    let identity = prepared_edit_execution_cache_identity(&receipt).expect("compact edit receipt");
    assert_eq!(
        identity.component(),
        format!(
            "{PREPARED_EDIT_EXECUTION_CACHE_SCHEMA}-{}",
            short_digest(canonical.as_bytes())
        )
    );
    assert!(!identity.component().contains("/Users"));

    let mut same_effective_route = receipt.clone();
    same_effective_route.adjustment_fell_back = false;
    same_effective_route.diagnostic = None;
    assert_eq!(
        identity,
        prepared_edit_execution_cache_identity(&same_effective_route)
            .expect("diagnostics do not alter effective route")
    );

    let mut metal = receipt;
    metal.cache_identity = canonical.replace("adjustment=cpu-v1", "adjustment=metal-v1");
    metal.adjustment_backend = EditPreviewBackend::Metal;
    assert_ne!(
        identity,
        prepared_edit_execution_cache_identity(&metal).expect("compact Metal edit receipt")
    );
}

#[test]
fn source_environment_distinguishes_router_policy_and_profile_root() {
    let automatic = source_environment_cache_identity("router-v1", None, None, None, None, None);
    let raw_frame = source_environment_cache_identity(
        "router-v1",
        Some(OsStr::new("raw-frame")),
        None,
        None,
        None,
        None,
    );
    let forced_cpu = source_environment_cache_identity(
        "router-v1",
        None,
        Some(OsStr::new("cpu")),
        None,
        None,
        None,
    );
    let other_router = source_environment_cache_identity("router-v2", None, None, None, None, None);
    let other_profile_root = source_environment_cache_identity(
        "router-v1",
        None,
        None,
        Some(OsStr::new("/profiles/alternate")),
        None,
        None,
    );
    let other_helper = source_environment_cache_identity(
        "router-v1",
        None,
        None,
        None,
        Some(OsStr::new("/helpers/alternate")),
        None,
    );

    assert_ne!(automatic, raw_frame);
    assert_ne!(automatic, forced_cpu);
    assert_ne!(automatic, other_router);
    assert_ne!(automatic, other_profile_root);
    assert_ne!(automatic, other_helper);
    assert_eq!(
        automatic,
        source_environment_cache_identity("router-v1", None, None, None, None, None)
    );
}

#[test]
fn prepared_source_reuse_compares_requested_not_effective_raw_plan_identity() {
    let requested_before_adjustment = "shadow-raw-plan-v1;intent=detail;quality=high";
    let effective_after_adjustment = "shadow-raw-plan-v1;intent=detail;quality=balanced";

    assert!(requested_raw_development_plan_cache_matches(
        requested_before_adjustment,
        requested_before_adjustment,
    ));
    // A provider can negotiate the high-quality request down to balanced. A
    // later balanced request must still negotiate and receive its own receipt,
    // rather than inheriting the prior high-quality request's provenance.
    assert!(!requested_raw_development_plan_cache_matches(
        requested_before_adjustment,
        effective_after_adjustment,
    ));
}
