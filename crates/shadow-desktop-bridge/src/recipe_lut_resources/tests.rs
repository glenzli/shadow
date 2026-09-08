use super::*;
use crate::{
    recipe_import_plan::RecipeImportPlan,
    recipe_v1::{GradeStackDraft, grade_stack_recipe_v1_snapshot},
};

fn cube() -> String {
    "TITLE \"Portable LUT\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n".into()
}

#[test]
fn export_import_survives_missing_source_lut_and_preserves_intensity() {
    let source = tempfile::tempdir().unwrap();
    let destination = tempfile::tempdir().unwrap();
    let text = cube();
    let id = format!("{:x}", Sha256::digest(text.as_bytes()));
    let path = source.path().join(format!("{id}.cube"));
    fs::write(&path, &text).unwrap();
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].fine.lut = LutEditParameters {
        resource_id: id.clone(),
        title: "Portable LUT".into(),
        managed_path: path.to_str().unwrap().into(),
        intensity: 0.37,
    };
    let resources = collect_lut_resources(&draft).unwrap();
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).unwrap();
    let document = ShadowRecipeDocument::new(None, snapshot)
        .unwrap()
        .with_lut_resources(resources)
        .unwrap();
    let bytes = document.to_pretty_json().unwrap();
    fs::remove_file(&path).unwrap();
    let decoded = ShadowRecipeDocument::from_json(&bytes).unwrap();
    let installed = install_lut_resources(&decoded, destination.path()).unwrap();
    assert_eq!(fs::read(&installed[&id]).unwrap(), text.as_bytes());
    let plan = RecipeImportPlan::from_document_with_luts(&decoded, &installed).unwrap();
    let projected = plan.finalize().unwrap();
    assert_eq!(projected.grade_nodes[0].fine.lut.resource_id, id);
    assert_eq!(projected.grade_nodes[0].fine.lut.intensity, 0.37);
    assert_ne!(
        projected.grade_nodes[0].fine.lut.managed_path,
        path.to_str().unwrap()
    );
    assert_eq!(
        install_lut_resources(&decoded, destination.path()).unwrap(),
        installed
    );
}

#[test]
fn malformed_or_misidentified_bundle_is_rejected_before_store_writes() {
    let root = tempfile::tempdir().unwrap();
    let store = root.path().join("not-created");
    for (id, text) in [
        ("a".repeat(64), cube()),
        (
            format!("{:x}", Sha256::digest(b"invalid cube")),
            "invalid cube".into(),
        ),
    ] {
        let resource = ShadowRecipeLutResource::new(id, text).unwrap();
        let document = ShadowRecipeDocument::new(None, shadow_domain::RecipeSnapshot::empty())
            .unwrap()
            .with_lut_resources(vec![resource])
            .unwrap();
        assert!(install_lut_resources(&document, &store).is_err());
        assert!(!store.exists());
    }
}

#[test]
fn conflicting_destination_bytes_are_preserved_and_rejected() {
    let root = tempfile::tempdir().unwrap();
    let text = cube();
    let id = format!("{:x}", Sha256::digest(text.as_bytes()));
    let path = root.path().join(format!("{id}.cube"));
    fs::write(&path, b"existing bytes").unwrap();
    let document = ShadowRecipeDocument::new(None, shadow_domain::RecipeSnapshot::empty())
        .unwrap()
        .with_lut_resources(vec![ShadowRecipeLutResource::new(id, text).unwrap()])
        .unwrap();
    assert!(install_lut_resources(&document, root.path()).is_err());
    assert_eq!(fs::read(path).unwrap(), b"existing bytes");
}
