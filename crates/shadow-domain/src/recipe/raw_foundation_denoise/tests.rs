use super::*;

#[test]
fn absent_node_is_the_non_destructive_identity() {
    let node = RawFoundationDenoise::default();

    assert!(!node.is_present());
    assert!(!node.is_bypassed());
    assert!(!node.is_enabled());
    assert!(!node.is_effective());
    assert_eq!(
        node.amount_percent(),
        RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT
    );
    assert_eq!(
        node.model(),
        RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0
    );
    let added_then_disabled = node.with_enabled(true).with_enabled(false);
    assert!(added_then_disabled.is_present());
    assert!(!added_then_disabled.is_enabled());
    assert!(!added_then_disabled.is_bypassed());
    assert_ne!(added_then_disabled, RawFoundationDenoise::default());
}

#[test]
fn public_model_identity_pins_package_graph_and_algorithm() {
    let model = RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0;

    assert_eq!(model.identity(), "rawnind-public-bayer-release-5.6.0");
    assert_eq!(
        model.package_sha256(),
        RawFoundationDenoiseModel::RAWNIND_PACKAGE_SHA256
    );
    assert_eq!(
        model.graph_sha256(),
        RawFoundationDenoiseModel::RAWNIND_BAYER_GRAPH_SHA256
    );
    assert_eq!(
        model.implementation_revision(),
        RawFoundationDenoiseModel::RAWNIND_IMPLEMENTATION_REVISION
    );
}

#[test]
fn enabled_node_has_one_exact_persisted_shape() {
    let node =
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0);
    let encoded = serde_json::to_string(&node).expect("serialize AI RAW denoise node");

    assert_eq!(
        encoded,
        r#"{"present":true,"enabled":true,"model":"raw_nind_public_bayer_release5_6_0"}"#
    );
    assert_eq!(
        serde_json::from_str::<RawFoundationDenoise>(&encoded)
            .expect("deserialize AI RAW denoise node"),
        node
    );
}

#[test]
fn amount_is_a_fast_persisted_blend_and_bypass_preserves_it() {
    let node =
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0)
            .with_amount_percent(42)
            .expect("valid amount");
    let ai_disabled = node.with_enabled(false);
    let hidden = node.with_bypassed(true);

    assert_eq!(node.amount_percent(), 42);
    assert!(node.is_effective());
    assert_eq!(ai_disabled.amount_percent(), 42);
    assert!(ai_disabled.is_present());
    assert!(!ai_disabled.is_effective());
    assert!(hidden.is_enabled());
    assert!(hidden.is_bypassed());
    assert!(!hidden.is_effective());
    assert_eq!(
        serde_json::to_string(&node).expect("serialize amount"),
        r#"{"present":true,"enabled":true,"model":"raw_nind_public_bayer_release5_6_0","amount_percent":42}"#
    );
    let legacy = serde_json::from_str::<RawFoundationDenoise>(
        r#"{"enabled":true,"model":"raw_nind_public_bayer_release5_6_0"}"#,
    )
    .expect("legacy full amount");
    assert!(legacy.is_present());
    assert!(legacy.is_enabled());
    assert_eq!(legacy.amount_percent(), 100);
    assert_eq!(
        RawFoundationDenoise::default()
            .with_amount_percent(101)
            .expect_err("reject invalid amount"),
        RecipeValidationError::InvalidRawFoundationDenoiseAmount(101)
    );
}

#[test]
fn added_bypassed_and_removed_are_distinct_persisted_states() {
    let added =
        RawFoundationDenoise::added(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0);
    assert!(added.is_present());
    assert!(!added.is_bypassed());
    assert!(!added.is_enabled());
    assert_eq!(
        serde_json::to_string(&added).expect("serialize added node"),
        r#"{"present":true,"enabled":false,"model":"raw_nind_public_bayer_release5_6_0"}"#
    );

    let bypassed = added.with_bypassed(true);
    assert!(bypassed.is_present());
    assert!(bypassed.is_bypassed());
    assert_eq!(
        serde_json::to_string(&bypassed).expect("serialize hidden node"),
        r#"{"present":true,"bypassed":true,"enabled":false,"model":"raw_nind_public_bayer_release5_6_0"}"#
    );

    let removed = bypassed.with_present(false);
    assert_eq!(removed, RawFoundationDenoise::absent());
    assert!(!removed.is_present());
}

#[test]
fn enabled_legacy_payload_infers_presence() {
    let legacy: RawFoundationDenoise =
        serde_json::from_str(r#"{"enabled":true,"model":"raw_nind_public_bayer_release5_6_0"}"#)
            .expect("decode legacy enabled denoise node");

    assert!(legacy.is_present());
    assert!(legacy.is_enabled());
}
