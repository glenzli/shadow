use shadow_bridge::{RawHighlightRecoveryIntent, RawPipelinePath, RawPipelineReceipt};
use shadow_domain::{
    PhotoFoundationNode, RawFoundationDenoise, RawFoundationDenoiseModel, RawTemperatureTint,
    RawWhiteBalance, RecipeInputSettings, RecipeOpticsSettings, RecipeSnapshot,
};

use super::*;

fn manual_white_balance() -> RawWhiteBalance {
    RawWhiteBalance::temperature_tint(
        RawTemperatureTint::new(6_400, 12).expect("manual temperature/tint"),
    )
}

fn receipt(
    path: RawPipelinePath,
    requested_plan: RawDevelopmentPlan,
    effective_plan: RawDevelopmentPlan,
) -> RawPipelineReceipt {
    RawPipelineReceipt {
        schema_version: RawPipelineReceipt::CURRENT_SCHEMA_VERSION,
        path,
        cache_identity: "raw-cache-fixture".to_owned(),
        pipeline_identity: "raw-pipeline-fixture".to_owned(),
        requested_plan,
        effective_plan,
        ..RawPipelineReceipt::default()
    }
}

#[test]
fn every_render_intent_binds_the_same_foundation_white_balance() {
    let white_balance = manual_white_balance();

    assert_eq!(
        preview_foundation_development_plan(white_balance).white_balance,
        white_balance
    );
    assert_eq!(
        detail_foundation_development_plan(white_balance).white_balance,
        white_balance
    );
    assert_eq!(
        export_foundation_development_plan(white_balance).white_balance,
        white_balance
    );
}

#[test]
fn resolved_contract_keeps_foundation_bypass_and_ai_visibility_independent() {
    let optics = RecipeOpticsSettings::new(true, false, true, false, false)
        .with_manual_corrections(8, -3, 5, -11, 63)
        .with_manual_profile("camera", "model", "lens", "profile");
    let ai =
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0);
    let foundation = PhotoFoundationNode::new(
        RecipeInputSettings::new(optics)
            .with_enabled(false)
            .with_raw_white_balance(manual_white_balance())
            .with_raw_ai_denoise(ai),
    );
    let snapshot = RecipeSnapshot::new_with_foundation(1, foundation, Vec::new())
        .expect("build Foundation source contract");

    let resolved = ResolvedFoundationDevelopment::from_snapshot(&snapshot);
    assert_eq!(
        resolved.preview_plan().white_balance,
        RawWhiteBalance::AsShot
    );
    assert!(!resolved.optics().enabled);
    assert!(!resolved.optics().correct_distortion);
    assert_eq!(resolved.optics().manual_vignetting_midpoint, 63);
    assert_eq!(resolved.optics().lens_profile_model, "profile");
    assert!(resolved.raw_ai_denoise().is_effective());
    assert_eq!(
        resolved.preview_plan().white_balance,
        resolved.detail_plan().white_balance
    );
    assert_eq!(
        resolved.detail_plan().white_balance,
        resolved.export_plan().white_balance
    );
}

#[test]
fn manual_white_balance_requires_an_exact_raw_frame_receipt() {
    let requested = preview_foundation_development_plan(manual_white_balance());
    let exact = receipt(RawPipelinePath::ShadowRawFrame, requested, requested);
    ensure_foundation_development_receipt(requested, &exact)
        .expect("exact RawFrame development receipt");

    let decoded = receipt(RawPipelinePath::DecodedRaster, requested, requested);
    let error = ensure_foundation_development_receipt(requested, &decoded)
        .expect_err("decoded RGB cannot execute manual sensor white balance");
    assert!(error.to_string().contains("sensor-domain RawFrame"));

    let as_shot_effective = preview_foundation_development_plan(RawWhiteBalance::AsShot);
    let changed = receipt(
        RawPipelinePath::ShadowRawFrame,
        requested,
        as_shot_effective,
    );
    assert!(
        ensure_foundation_development_receipt(requested, &changed).is_err(),
        "negotiation may not discard the requested absolute white balance"
    );
}

#[test]
fn manual_white_balance_rejects_rgb_isolation_while_as_shot_allows_it() {
    let manual = preview_foundation_development_plan(manual_white_balance());
    assert!(ensure_foundation_allows_rgb_fallback(manual).is_err());
    assert!(ensure_foundation_allows_rgb_fallback(RawDevelopmentPlan::preview()).is_ok());
}

#[test]
fn highlight_repair_requires_and_preserves_the_sensor_domain_rawframe_plan() {
    let snapshot = RecipeSnapshot::new_with_input_settings(
        1,
        RecipeInputSettings::default().with_raw_highlight_repair_enabled(true),
        Vec::new(),
    )
    .expect("build highlight repair Foundation");
    let resolved = ResolvedFoundationDevelopment::from_snapshot(&snapshot);
    for plan in [
        resolved.preview_plan(),
        resolved.detail_plan(),
        resolved.export_plan(),
    ] {
        assert_eq!(
            plan.highlight_recovery,
            RawHighlightRecoveryIntent::Aggressive
        );
        assert!(ensure_foundation_allows_rgb_fallback(plan).is_err());
        ensure_foundation_development_receipt(
            plan,
            &receipt(RawPipelinePath::ShadowRawFrame, plan, plan),
        )
        .expect("exact RawFrame highlight repair receipt");
    }

    let requested = resolved.preview_plan();
    let degraded = receipt(
        RawPipelinePath::ShadowRawFrame,
        requested,
        RawDevelopmentPlan::preview(),
    );
    assert!(ensure_foundation_development_receipt(requested, &degraded).is_err());
}
