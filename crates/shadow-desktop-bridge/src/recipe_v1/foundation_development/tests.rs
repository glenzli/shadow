use shadow_bridge::{RawPipelinePath, RawPipelineReceipt};
use shadow_domain::{RawTemperatureTint, RawWhiteBalance};

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
