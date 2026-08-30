use shadow_domain::{
    MaskDefinition, PhotoCanvasNode, PhotoFoundationNode, RecipeInputSettings,
    SemanticMaskAggregation, SemanticMaskIntent, ShadowRecipeDocument, UnitInterval,
};

use super::{make_destination_safe, preview_shadow_recipe_document};
use crate::recipe_v1::{
    GradeStackDraft, PreservedManagedRasterSettings, decode_grade_stack_draft_recipe_v1,
    grade_stack_recipe_v1_snapshot,
};

#[test]
fn import_preview_keeps_grade_controls_and_normalized_masks_only() {
    let mut source = GradeStackDraft::default();
    let source_node_id = source.grade_nodes[0].recipe_v1_identity.grade_node_id;
    source.grade_nodes[0].basic.exposure_stops = 1.25;
    source.grade_nodes[0].local_mask = Some(
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.4).expect("center x"),
            UnitInterval::new(0.6).expect("center y"),
            UnitInterval::new(0.25).expect("radius x"),
            UnitInterval::new(0.3).expect("radius y"),
            UnitInterval::new(0.5).expect("feather"),
            false,
        )
        .expect("radial mask"),
    );
    source.foundation =
        PhotoFoundationNode::new(RecipeInputSettings::default().with_enabled(false));
    source.canvas = PhotoCanvasNode::added();

    let snapshot = grade_stack_recipe_v1_snapshot(&source, None).expect("source snapshot");
    let document = ShadowRecipeDocument::new(Some("Soft portrait"), snapshot)
        .expect("document")
        .to_pretty_json()
        .expect("JSON");
    let preview = preview_shadow_recipe_document(&document).expect("preview");
    let portable =
        decode_grade_stack_draft_recipe_v1(&preview.portable_settings).expect("portable settings");

    assert_eq!(preview.label, "Soft portrait");
    assert_eq!(preview.grade_node_count, 1);
    assert_eq!(preview.portable_mask_count, 1);
    assert!(preview.foundation_omitted);
    assert!(preview.canvas_omitted);
    assert_eq!(portable.grade_nodes[0].basic.exposure_stops, 1.25);
    assert_ne!(
        portable.grade_nodes[0].recipe_v1_identity.grade_node_id,
        source_node_id
    );
    assert!(matches!(
        portable.grade_nodes[0].local_mask,
        Some(MaskDefinition::RadialGradient { .. })
    ));
    assert_eq!(portable.foundation, PhotoFoundationNode::default());
    assert_eq!(portable.canvas, PhotoCanvasNode::identity());
}

#[test]
fn managed_mask_pixels_are_removed_and_the_node_is_bypassed() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].preserved_managed_raster = Some(PreservedManagedRasterSettings {
        expansion_percent: -4,
        feather_percent: 18,
        invert: false,
        semantic_intent: Some(
            SemanticMaskIntent::new("the main subject", 3, 54, SemanticMaskAggregation::Union)
                .expect("semantic intent"),
        ),
    });
    source.grade_nodes[0].fine.lut.resource_id = "lut-resource".to_owned();
    source.grade_nodes[0].fine.lut.title = "Local LUT".to_owned();
    source.grade_nodes[0].fine.lut.managed_path = "/private/cache/lut.cube".to_owned();

    let counts = make_destination_safe(&mut source).expect("safe projection");

    assert_eq!(counts.managed_mask_node_count, 1);
    assert_eq!(counts.semantic_mask_intent_count, 1);
    assert_eq!(counts.removed_lut_count, 1);
    assert!(!source.grade_nodes[0].enabled);
    assert!(source.grade_nodes[0].local_mask.is_none());
    assert!(source.grade_nodes[0].preserved_managed_raster.is_none());
    assert!(source.grade_nodes[0].fine.lut.resource_id.is_empty());
    assert!(source.grade_nodes[0].fine.lut.managed_path.is_empty());
}

#[test]
fn arbitrary_json_is_not_treated_as_an_importable_recipe() {
    let error = preview_shadow_recipe_document(br#"{"format":"not-shadow"}"#)
        .expect_err("must reject non-Recipe JSON");
    assert!(error.to_string().contains("decode Shadow Recipe document"));
}
