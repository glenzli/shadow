use super::*;

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
fn manual_raw_white_balance_has_one_scale_invariant_camera_neutral_identity() {
    let neutral = RawCameraNeutral::new(0.5, 1.0, 0.25).expect("camera neutral");
    let scaled = RawCameraNeutral::new(1.0, 2.0, 0.5).expect("same scaled neutral");

    assert_eq!(neutral, scaled);
    assert_eq!(neutral.red_millionths(), 500_000);
    assert_eq!(neutral.green_millionths(), RAW_CAMERA_NEUTRAL_MILLIONTHS);
    assert_eq!(neutral.blue_millionths(), 250_000);
    assert_eq!(neutral.red().to_bits(), 0.5_f64.to_bits());
    assert_eq!(neutral.green().to_bits(), 1.0_f64.to_bits());
    assert_eq!(neutral.blue().to_bits(), 0.25_f64.to_bits());

    let white_balance = RawWhiteBalance::camera_neutral(neutral);
    assert_eq!(white_balance.neutral(), Some(neutral));
    let encoded = serde_json::to_string(&white_balance).expect("serialize RAW white balance");
    assert_eq!(
        encoded,
        r#"{"mode":"camera_neutral","neutral":{"red_millionths":500000,"blue_millionths":250000}}"#
    );
    assert_eq!(
        serde_json::from_str::<RawWhiteBalance>(&encoded).expect("deserialize RAW white balance"),
        white_balance
    );
}

#[test]
fn invalid_or_noncanonical_camera_neutrals_fail_closed() {
    for invalid in [
        RawCameraNeutral::new(f64::NAN, 1.0, 1.0),
        RawCameraNeutral::new(1.0, 0.0, 1.0),
        RawCameraNeutral::new(65.0, 1.0, 1.0),
        RawCameraNeutral::from_millionths(0, RAW_CAMERA_NEUTRAL_MILLIONTHS),
    ] {
        assert!(matches!(
            invalid,
            Err(RecipeValidationError::InvalidRawCameraNeutral { .. })
        ));
    }

    let invalid_json =
        r#"{"mode":"camera_neutral","neutral":{"red_millionths":0,"blue_millionths":1000000}}"#;
    assert!(serde_json::from_str::<RawWhiteBalance>(invalid_json).is_err());
}

#[test]
fn foundation_omits_as_shot_but_persists_manual_raw_white_balance() {
    let default_json =
        serde_json::to_string(&PhotoFoundationNode::default()).expect("default Foundation");
    assert!(!default_json.contains("raw_white_balance"));

    let neutral = RawCameraNeutral::new(0.75, 1.0, 0.5).expect("manual neutral");
    let foundation = PhotoFoundationNode::new(
        RecipeInputSettings::default()
            .with_raw_white_balance(RawWhiteBalance::camera_neutral(neutral)),
    );
    assert_eq!(
        foundation.raw_white_balance(),
        RawWhiteBalance::camera_neutral(neutral)
    );
    let encoded = serde_json::to_string(&foundation).expect("manual Foundation");
    assert!(encoded.contains("\"raw_white_balance\""));
    assert_eq!(
        serde_json::from_str::<PhotoFoundationNode>(&encoded)
            .expect("manual Foundation round trip"),
        foundation
    );
}
