use super::*;

#[test]
fn disabled_node_is_the_non_destructive_identity() {
    let node = RawFoundationDenoise::default();

    assert!(!node.is_enabled());
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
