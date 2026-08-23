use super::*;

#[test]
fn recipe_rejects_an_incomplete_manual_optics_identity() {
    let optics = RecipeOpticsSettings {
        camera_profile_maker: "Pentax".to_owned(),
        camera_profile_model: "K10D".to_owned(),
        lens_profile_maker: String::new(),
        lens_profile_model: String::new(),
        ..RecipeOpticsSettings::default()
    };
    assert_eq!(
        optics.validate(),
        Err(RecipeValidationError::IncompleteOpticsProfile)
    );
}

#[test]
fn legacy_input_settings_default_to_an_enabled_foundation() {
    let decoded: RecipeInputSettings =
        serde_json::from_str(r#"{"optics":{}}"#).expect("legacy input settings");
    assert!(decoded.enabled());
    let encoded = serde_json::to_value(&decoded).expect("default input settings");
    assert!(encoded.get("enabled").is_none());
    assert!(encoded.get("raw_highlight_repair_enabled").is_none());
}

#[test]
fn historical_highlight_repair_bit_remains_round_trip_compatible() {
    let enabled = RecipeInputSettings::default().with_raw_highlight_repair_enabled(true);
    assert!(enabled.raw_highlight_repair_enabled());
    let encoded = serde_json::to_value(&enabled).expect("serialize enabled highlight repair");
    assert_eq!(
        encoded
            .get("raw_highlight_repair_enabled")
            .and_then(serde_json::Value::as_bool),
        Some(true)
    );
}
