use serde_json::Value;

use super::*;

#[test]
fn document_round_trip_preserves_label_snapshot_and_digest() {
    let snapshot = RecipeSnapshot::empty();
    let document = ShadowRecipeDocument::new(Some("Natural portrait"), snapshot.clone())
        .expect("valid document");
    let encoded = document.to_pretty_json().expect("encode document");
    let decoded = ShadowRecipeDocument::from_json(&encoded).expect("decode document");

    assert_eq!(decoded.label(), Some("Natural portrait"));
    assert_eq!(decoded.snapshot(), &snapshot);
    let json: Value = serde_json::from_slice(&encoded).expect("JSON");
    assert_eq!(json["format"], SHADOW_RECIPE_FORMAT);
    assert_eq!(
        json["document_version"],
        CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION
    );
    assert_eq!(json["snapshot_blake3"].as_str().map(str::len), Some(64));
}

#[test]
fn changed_snapshot_is_rejected_before_import() {
    let document = ShadowRecipeDocument::new(None, RecipeSnapshot::empty()).expect("document");
    let mut json: Value =
        serde_json::from_slice(&document.to_pretty_json().expect("encode")).expect("JSON");
    json["snapshot"]["schema_version"] = Value::from(99_u32);

    let error =
        ShadowRecipeDocument::from_json(&serde_json::to_vec(&json).expect("tampered document"))
            .expect_err("tampering must fail");
    assert!(matches!(
        error,
        ShadowRecipeDocumentError::RecipeSchemaMismatch { .. }
            | ShadowRecipeDocumentError::InvalidRecipe(_)
            | ShadowRecipeDocumentError::DigestMismatch
    ));
}

#[test]
fn format_version_and_label_are_fail_closed() {
    let document = ShadowRecipeDocument::new(None, RecipeSnapshot::empty()).expect("document");
    let mut json: Value =
        serde_json::from_slice(&document.to_pretty_json().expect("encode")).expect("JSON");
    json["document_version"] = Value::from(2_u32);
    assert!(matches!(
        ShadowRecipeDocument::from_json(&serde_json::to_vec(&json).expect("JSON")),
        Err(ShadowRecipeDocumentError::UnsupportedDocumentVersion(2))
    ));

    assert!(matches!(
        ShadowRecipeDocument::new(Some(" invalid "), RecipeSnapshot::empty()),
        Err(ShadowRecipeDocumentError::InvalidLabel)
    ));
}

#[test]
fn oversized_documents_are_rejected_without_parsing() {
    let oversized = vec![b' '; MAX_SHADOW_RECIPE_DOCUMENT_BYTES + 1];
    assert!(matches!(
        ShadowRecipeDocument::from_json(&oversized),
        Err(ShadowRecipeDocumentError::DocumentTooLarge)
    ));
}

#[test]
fn oversized_documents_are_also_rejected_when_encoding() {
    let oversized = vec![b' '; MAX_SHADOW_RECIPE_DOCUMENT_BYTES + 1];
    assert!(matches!(
        checked_document_bytes(oversized),
        Err(ShadowRecipeDocumentError::DocumentTooLarge)
    ));
}
