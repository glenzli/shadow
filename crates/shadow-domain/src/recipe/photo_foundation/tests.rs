use super::*;
use crate::RawFoundationDenoise;

#[test]
fn foundation_preserves_the_recipe_v1_input_settings_payload() {
    let input_settings =
        RecipeInputSettings::new(RecipeOpticsSettings::new(true, true, false, true, false));
    let foundation = PhotoFoundationNode::new(input_settings.clone());

    assert_eq!(foundation.input_settings(), &input_settings);
    assert_eq!(foundation.optics(), input_settings.optics());
    assert_eq!(
        serde_json::to_value(&foundation).expect("serialize Foundation"),
        serde_json::to_value(&input_settings).expect("serialize Recipe v1 input settings")
    );
}

#[test]
fn foundation_reset_is_an_identity_value_not_a_missing_node() {
    let foundation = PhotoFoundationNode::default();

    assert!(foundation.is_default());
    foundation.validate().expect("default Foundation is valid");
    assert_eq!(
        foundation.into_input_settings(),
        RecipeInputSettings::default()
    );
}

#[test]
fn manual_raw_white_balance_has_one_human_facing_temperature_tint_identity() {
    let value = RawTemperatureTint::new(5_500, 12).expect("temperature and tint");
    assert_eq!(value.temperature_kelvin(), 5_500);
    assert_eq!(value.tint(), 12);

    let white_balance = RawWhiteBalance::temperature_tint(value);
    assert_eq!(white_balance.authored_value(), Some(value));
    let encoded = serde_json::to_string(&white_balance).expect("serialize RAW white balance");
    assert_eq!(
        encoded,
        r#"{"mode":"temperature_tint","value":{"temperature_kelvin":5500,"tint":12}}"#
    );
    assert_eq!(
        serde_json::from_str::<RawWhiteBalance>(&encoded).expect("deserialize RAW white balance"),
        white_balance
    );
}

#[test]
fn invalid_temperature_or_tint_fails_closed() {
    for invalid in [
        RawTemperatureTint::new(1_999, 0),
        RawTemperatureTint::new(25_001, 0),
        RawTemperatureTint::new(5_500, -151),
        RawTemperatureTint::new(5_500, 151),
    ] {
        assert!(invalid.is_err());
    }

    let invalid_json =
        r#"{"mode":"temperature_tint","value":{"temperature_kelvin":1200,"tint":0}}"#;
    assert!(serde_json::from_str::<RawWhiteBalance>(invalid_json).is_err());
}

#[test]
fn foundation_omits_as_shot_but_persists_manual_raw_white_balance() {
    let default_json =
        serde_json::to_string(&PhotoFoundationNode::default()).expect("default Foundation");
    assert!(!default_json.contains("raw_white_balance"));

    let value = RawTemperatureTint::new(6_200, -8).expect("manual white balance");
    let foundation = PhotoFoundationNode::new(
        RecipeInputSettings::default()
            .with_raw_white_balance(RawWhiteBalance::temperature_tint(value)),
    );
    assert_eq!(
        foundation.raw_white_balance(),
        RawWhiteBalance::temperature_tint(value)
    );
    let encoded = serde_json::to_string(&foundation).expect("manual Foundation");
    assert!(encoded.contains("\"raw_white_balance\""));
    assert_eq!(
        serde_json::from_str::<PhotoFoundationNode>(&encoded)
            .expect("manual Foundation round trip"),
        foundation
    );
}

#[test]
fn disabled_foundation_is_persisted_and_bypasses_optional_source_interpretation() {
    let white_balance = RawWhiteBalance::temperature_tint(
        RawTemperatureTint::new(4_300, 18).expect("white balance"),
    );
    let denoise = RawFoundationDenoise::enabled(
        crate::RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
    );
    let foundation = PhotoFoundationNode::new(
        RecipeInputSettings::default()
            .with_enabled(false)
            .with_raw_white_balance(white_balance)
            .with_raw_ai_denoise(denoise),
    );

    assert!(!foundation.enabled());
    assert_eq!(foundation.raw_white_balance(), white_balance);
    assert_eq!(
        foundation.effective_raw_white_balance(),
        RawWhiteBalance::AsShot
    );
    assert_eq!(foundation.input_settings().raw_ai_denoise(), denoise);

    let encoded = serde_json::to_string(&foundation).expect("disabled Foundation");
    assert!(encoded.contains(r#""enabled":false"#));
    assert_eq!(
        serde_json::from_str::<PhotoFoundationNode>(&encoded)
            .expect("disabled Foundation round trip"),
        foundation
    );
}

#[test]
fn foundation_omits_disabled_ai_denoise_and_persists_one_enabled_slot() {
    let default_json =
        serde_json::to_string(&PhotoFoundationNode::default()).expect("default Foundation");
    assert!(!default_json.contains("raw_ai_denoise"));

    let denoise = RawFoundationDenoise::enabled(
        crate::RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
    );
    let foundation =
        PhotoFoundationNode::new(RecipeInputSettings::default().with_raw_ai_denoise(denoise));

    assert_eq!(foundation.input_settings().raw_ai_denoise(), denoise);
    let encoded = serde_json::to_string(&foundation).expect("AI denoise Foundation");
    assert!(encoded.contains("\"raw_ai_denoise\""));
    assert_eq!(
        serde_json::from_str::<PhotoFoundationNode>(&encoded)
            .expect("AI denoise Foundation round trip"),
        foundation
    );
}
