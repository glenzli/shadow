use super::*;
use crate::{
    CommitEditRepository, CreatePhotoVariant, EditObjectPackWrite, EditRepositoryRefUpdate,
    RecipeRefExpectation, RecipeRefKind, RecipeRefTarget, RegisterAsset,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, AssetLocation, BlendMode, EditGraph, EditObject,
    EditObjectPack, EditRepositoryCommit, EditRepositoryCommitPayloadV1,
    EditRepositoryRefExpectation, EditRepositoryRefKind, ImageDomain, LibraryRootV1, NodeId,
    NodeInput, OperationDescriptor, OperationId, ParameterBlock, Platform, PortType,
    ProcessingStage, RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind, UnitInterval,
};

fn graph() -> EditGraph {
    let id = NodeId::new_v7();
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new("shadow.test.shared-grade").unwrap(),
        1,
        "catalog-test-v1",
        ProcessingStage::CreativeColor,
        vec![image],
        image,
        None,
    )
    .unwrap();
    let node = AdjustmentNode::new(
        id,
        operation,
        vec![NodeInput::GraphInput { index: 0 }],
        ParameterBlock::default(),
        None,
    )
    .unwrap();
    EditGraph::new(1, vec![image], vec![node], id).unwrap()
}

fn layer(
    layer_id: LayerId,
    revision_id: LayerRevisionId,
    follows: bool,
) -> shadow_domain::LayerInstance {
    shadow_domain::LayerInstance::new(
        LayerInstanceId::new_v7(),
        "Shared",
        AdjustmentScope::Photo,
        LayerContent::Shared {
            layer_id,
            revision: LayerRevisionSelector::Pinned(revision_id),
            graph: graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .unwrap()
    .with_shared_follow_head(follows)
    .unwrap()
}

fn register(catalog: &mut Catalog, path: &str) -> (PhotoId, SharedGradeSourceGuard) {
    let asset = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 16,
            modified_at_ms: Some(100),
            now_ms: 100,
        })
        .unwrap();
    (
        asset.photo_id,
        SharedGradeSourceGuard {
            representation_id: asset.representation_id,
            source_path: path.into(),
            fingerprint: RepresentationFingerprint {
                byte_len: 16,
                modified_at_ms: Some(100),
            },
        },
    )
}

fn working(
    photo_id: PhotoId,
    snapshot: RecipeSnapshot,
    parent: Option<RecipeCommitId>,
) -> CommitRecipe {
    CommitRecipe {
        photo_id,
        commit: RecipeCommit::new(
            RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            parent.into_iter().collect(),
            snapshot,
            None,
            100,
        )
        .unwrap(),
        update_refs: vec![RecipeRefTarget {
            name: "working".into(),
            kind: RecipeRefKind::Working,
            expectation: Some(
                parent.map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
            ),
        }],
    }
}

fn repository(catalog: &mut Catalog) -> EditCommitId {
    let root = LibraryRootV1 {
        photo_recipes: None,
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .unwrap();
    let root_id = root.object().id();
    catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![root],
            created_at_ms: 100,
        })
        .unwrap();
    let commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root: root_id,
        parents: Vec::new(),
        message: None,
        created_at_ms: 100,
    })
    .unwrap();
    let id = commit.id();
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit,
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 100,
            }],
        })
        .unwrap();
    id
}

#[test]
fn bounded_pages_continue_through_empty_pages_and_include_all_exact_instances() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let layer_id = LayerId::new_v7();
    let old = LayerRevisionId::new_v7();
    let latest = LayerRevisionId::new_v7();
    let mut photos = (0..3)
        .map(|i| register(&mut catalog, &format!("/photos/page-{i}.dng")).0)
        .collect::<Vec<_>>();
    photos.sort();
    for (position, photo_id) in photos.iter().copied().enumerate() {
        let layers = if position == 1 {
            vec![
                layer(layer_id, old, true),
                layer(layer_id, old, true),
                layer(layer_id, latest, true),
                layer(layer_id, old, false),
            ]
        } else {
            Vec::new()
        };
        let request = working(photo_id, RecipeSnapshot::new(1, layers).unwrap(), None);
        catalog.commit_recipe(&request).unwrap();
    }
    let first = catalog
        .shared_grade_linked_working_page(layer_id, latest, None, 1)
        .unwrap();
    assert!(first.records.is_empty());
    assert_eq!(first.next_photo_id, Some(photos[0]));
    let second = catalog
        .shared_grade_linked_working_page(layer_id, latest, first.next_photo_id, 1)
        .unwrap();
    assert_eq!(second.records.len(), 1);
    assert_eq!(second.records[0].photo_id, photos[1]);
    assert_eq!(second.records[0].instances.len(), 3);
    assert_ne!(
        second.records[0].instances[0].instance_id,
        second.records[0].instances[1].instance_id
    );
    let third = catalog
        .shared_grade_linked_working_page(layer_id, latest, second.next_photo_id, 1)
        .unwrap();
    assert!(third.records.is_empty());
    assert!(third.next_photo_id.is_none());
    assert!(
        catalog
            .shared_grade_linked_working_page(layer_id, latest, None, 0)
            .is_err()
    );
}

#[test]
fn inactive_variants_retain_their_frozen_subscription_without_joining_active_pages() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let (photo, _) = register(&mut catalog, "/photos/variant.dng");
    let layer_id = LayerId::new_v7();
    let revision_id = LayerRevisionId::new_v7();
    let initial = working(
        photo,
        RecipeSnapshot::new(1, vec![layer(layer_id, revision_id, true)]).unwrap(),
        None,
    );
    catalog.commit_recipe(&initial).unwrap();
    catalog
        .create_photo_variant(&CreatePhotoVariant {
            id: PhotoVariantId::new_v7(),
            photo_id: photo,
            name: "Neutral".into(),
            source_commit_id: None,
            activate: true,
            created_at_ms: 200,
        })
        .unwrap();
    let page = catalog
        .shared_grade_linked_working_page(layer_id, LayerRevisionId::new_v7(), None, 10)
        .unwrap();
    assert!(page.records.is_empty());
    assert!(
        catalog
            .recipe_commit(photo, initial.commit.id())
            .unwrap()
            .unwrap()
            .commit
            .snapshot()
            .layers()[0]
            .shared_follow_head()
    );
}

#[test]
fn guarded_shared_write_rejects_source_variant_and_library_drift_without_a_commit() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let (photo, source) = register(&mut catalog, "/photos/guard.dng");
    let library = repository(&mut catalog);
    let initial = working(photo, RecipeSnapshot::empty(), None);
    catalog.commit_recipe(&initial).unwrap();
    let variant = catalog.photo_variants(photo).unwrap()[0].id;
    let request = CommitSharedGradeLinkUpdate {
        recipe: working(photo, RecipeSnapshot::empty(), Some(initial.commit.id())),
        expected_variant_id: variant,
        expected_library_commit_id: library,
        library_ref_name: "heads/main".into(),
        source,
    };
    let mut stale = request.clone();
    stale.source.fingerprint.byte_len += 1;
    assert_eq!(
        catalog.commit_shared_grade_link_update(&stale).unwrap(),
        SharedGradeLinkWriteResult::SourceChanged
    );
    stale = request.clone();
    stale.expected_library_commit_id = EditCommitId::from_bytes([7; 32]);
    assert_eq!(
        catalog.commit_shared_grade_link_update(&stale).unwrap(),
        SharedGradeLinkWriteResult::HeadChanged
    );
    catalog
        .create_photo_variant(&CreatePhotoVariant {
            id: PhotoVariantId::new_v7(),
            photo_id: photo,
            name: "Alternate".into(),
            source_commit_id: Some(initial.commit.id()),
            activate: true,
            created_at_ms: 200,
        })
        .unwrap();
    assert_eq!(
        catalog.commit_shared_grade_link_update(&request).unwrap(),
        SharedGradeLinkWriteResult::HeadChanged
    );
    assert!(
        catalog
            .recipe_commit(photo, request.recipe.commit.id())
            .unwrap()
            .is_none()
    );
    assert_eq!(
        catalog
            .recipe_ref(photo, "working")
            .unwrap()
            .unwrap()
            .commit_id,
        initial.commit.id()
    );
}

#[test]
fn immutable_revision_lookup_preserves_old_numbers_and_checks_object_integrity() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let layer_id = LayerId::new_v7();
    let revision = LayerRevision::new(
        LayerRevisionId::new_v7(),
        layer_id,
        1,
        None,
        "First",
        graph(),
    )
    .unwrap();
    let object =
        EditObject::from_canonical_json(EditObjectKind::GradeNodeRevision, 1, &revision).unwrap();
    let pack = EditObjectPack::new(object, Vec::new()).unwrap();
    catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![pack],
            created_at_ms: 100,
        })
        .unwrap();
    let found = catalog
        .shared_grade_revision_by_id(layer_id, revision.id())
        .unwrap()
        .unwrap();
    assert_eq!(found, revision);
    assert!(
        catalog
            .shared_grade_revision_by_id(LayerId::new_v7(), revision.id())
            .unwrap()
            .is_none()
    );
}
