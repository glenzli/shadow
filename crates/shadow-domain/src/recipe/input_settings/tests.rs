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
}
