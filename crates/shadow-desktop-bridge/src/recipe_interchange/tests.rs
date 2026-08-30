use shadow_domain::{
    EntityId, ManagedRasterMask, MaskComponentId, MaskComponentOperation, MaskDefinition,
    PhotoCanvasNode, PhotoFoundationNode, RasterMaskEncoding, RecipeInputSettings,
    SemanticMaskAggregation, SemanticMaskIntent, ShadowRecipeDocument, UnitInterval,
};

use super::{make_destination_safe, preview_shadow_recipe_document};
use crate::recipe_v1::{
    CompositeMaskDraft, GradeStackDraft, MaskComponentDraft, MaskComponentDraftDefinition,
    PreservedManagedRasterSettings, decode_grade_stack_draft_recipe_v1,
    encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
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
    assert!(portable.grade_nodes[0].local_mask.is_none());
    let portable_mask = portable.grade_nodes[0]
        .composite_mask
        .as_ref()
        .expect("portable mask component");
    assert_eq!(portable_mask.components.len(), 1);
    assert_eq!(
        portable_mask.components[0].operation,
        MaskComponentOperation::Base
    );
    assert!(matches!(
        portable_mask.components[0].definition,
        MaskComponentDraftDefinition::Definition(MaskDefinition::RadialGradient { .. })
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
fn portable_composite_components_keep_order_operations_and_count() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].composite_mask = Some(CompositeMaskDraft {
        components: vec![
            component(
                MaskComponentOperation::Base,
                MaskDefinition::linear_gradient(unit(0.1), unit(0.2), unit(0.8), unit(0.7), false)
                    .expect("linear mask"),
            ),
            component(
                MaskComponentOperation::Add,
                MaskDefinition::radial_gradient(
                    unit(0.5),
                    unit(0.5),
                    unit(0.3),
                    unit(0.2),
                    unit(0.4),
                    false,
                )
                .expect("radial mask"),
            ),
            component(
                MaskComponentOperation::Intersect,
                MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.1), false)
                    .expect("luminance mask"),
            ),
        ],
        invert: true,
    });

    let counts = make_destination_safe(&mut source).expect("safe projection");

    assert_eq!(counts.portable_mask_count, 3);
    assert_eq!(counts.managed_mask_node_count, 0);
    assert_eq!(counts.semantic_mask_intent_count, 0);
    assert!(source.grade_nodes[0].enabled);
    let composite = source.grade_nodes[0]
        .composite_mask
        .as_ref()
        .expect("portable composite remains");
    assert!(composite.invert);
    assert_eq!(
        composite
            .components
            .iter()
            .map(|component| component.operation)
            .collect::<Vec<_>>(),
        vec![
            MaskComponentOperation::Base,
            MaskComponentOperation::Add,
            MaskComponentOperation::Intersect,
        ]
    );
    encode_grade_stack_draft_recipe_v1(source).expect("portable composite encodes");
}

#[test]
fn mixed_composite_drops_the_complete_topology_and_never_carries_source_raster_identity() {
    let mut source = GradeStackDraft::default();
    let (source_raster, source_hash) = source_managed_raster();
    let source_object_id = source_raster.store_object_id().to_owned();
    source.grade_nodes[0].composite_mask = Some(CompositeMaskDraft {
        components: vec![
            component(
                MaskComponentOperation::Base,
                MaskDefinition::radial_gradient(
                    unit(0.5),
                    unit(0.5),
                    unit(0.3),
                    unit(0.3),
                    unit(0.5),
                    false,
                )
                .expect("radial mask"),
            ),
            MaskComponentDraft {
                id: new_component_id(),
                operation: MaskComponentOperation::Subtract,
                enabled: true,
                definition: MaskComponentDraftDefinition::PreservedManagedRaster(
                    PreservedManagedRasterSettings {
                        expansion_percent: -4,
                        feather_percent: 18,
                        invert: false,
                        semantic_intent: Some(
                            SemanticMaskIntent::new(
                                "the main subject",
                                3,
                                54,
                                SemanticMaskAggregation::Union,
                            )
                            .expect("semantic intent"),
                        ),
                    },
                ),
            },
            component(
                MaskComponentOperation::Intersect,
                MaskDefinition::managed_raster(source_raster, false)
                    .expect("non-semantic managed mask"),
            ),
        ],
        invert: false,
    });

    let counts = make_destination_safe(&mut source).expect("safe projection");

    assert_eq!(counts.managed_mask_node_count, 1);
    assert_eq!(counts.semantic_mask_intent_count, 1);
    // The portable Base is not reported as imported because preserving it
    // without the later Subtract/Intersect leaves would change authored
    // composite meaning.
    assert_eq!(counts.portable_mask_count, 0);
    assert!(!source.grade_nodes[0].enabled);
    assert!(source.grade_nodes[0].composite_mask.is_none());
    assert!(source.grade_nodes[0].local_mask.is_none());
    assert!(source.grade_nodes[0].preserved_managed_raster.is_none());

    let snapshot = grade_stack_recipe_v1_snapshot(&source, None).expect("safe snapshot");
    let document = ShadowRecipeDocument::new(None, snapshot)
        .expect("safe document")
        .to_pretty_json()
        .expect("safe JSON");
    let document = String::from_utf8(document).expect("UTF-8 JSON");
    assert!(!document.contains(&source_object_id));
    assert!(!document.contains(&source_hash));
}

#[test]
fn arbitrary_json_is_not_treated_as_an_importable_recipe() {
    let error = preview_shadow_recipe_document(br#"{"format":"not-shadow"}"#)
        .expect_err("must reject non-Recipe JSON");
    assert!(error.to_string().contains("decode Shadow Recipe document"));
}

fn component(operation: MaskComponentOperation, definition: MaskDefinition) -> MaskComponentDraft {
    MaskComponentDraft {
        id: new_component_id(),
        operation,
        enabled: true,
        definition: MaskComponentDraftDefinition::Definition(definition),
    }
}

fn new_component_id() -> MaskComponentId {
    MaskComponentId::from_uuid(uuid::Uuid::new_v4())
}

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("unit interval")
}

fn source_managed_raster() -> (ManagedRasterMask, String) {
    let content_hash = format!("ab{}", "1".repeat(62));
    let store_object_id = format!(
        "objects/v1/b3/{}/{}",
        &content_hash[..2],
        &content_hash[2..]
    );
    let raster = ManagedRasterMask::new(
        store_object_id,
        1,
        content_hash.clone(),
        16,
        4,
        4,
        400,
        300,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    (raster, content_hash)
}
