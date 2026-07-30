use super::*;

#[test]
fn disabled_node_is_the_non_destructive_identity() {
    let node = RawFoundationDenoise::default();

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
    assert_eq!(
        node.with_enabled(true).with_enabled(false),
        RawFoundationDenoise::default()
    );
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
        r#"{"enabled":true,"model":"raw_nind_public_bayer_release5_6_0"}"#
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
    let bypassed = node.with_enabled(false);

    assert_eq!(node.amount_percent(), 42);
    assert!(node.is_effective());
    assert_eq!(bypassed.amount_percent(), 42);
    assert!(!bypassed.is_effective());
    assert_eq!(
        serde_json::to_string(&node).expect("serialize amount"),
        r#"{"enabled":true,"model":"raw_nind_public_bayer_release5_6_0","amount_percent":42}"#
    );
    assert_eq!(
        serde_json::from_str::<RawFoundationDenoise>(
            r#"{"enabled":true,"model":"raw_nind_public_bayer_release5_6_0"}"#
        )
        .expect("legacy full amount"),
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0)
    );
    assert_eq!(
        RawFoundationDenoise::default()
            .with_amount_percent(101)
            .expect_err("reject invalid amount"),
        RecipeValidationError::InvalidRawFoundationDenoiseAmount(101)
    );
}
