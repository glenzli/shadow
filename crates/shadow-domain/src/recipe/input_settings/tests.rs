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
