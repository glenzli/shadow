use super::*;
use crate::{
    open_desktop_session,
    recipe_v1::{GradeNodeDraft, GradeNodeRecipeV1Identity, grade_node_draft_from_shared_revision},
};
use shadow_catalog::RegisterAsset;
use shadow_domain::{MaskDefinition, RepresentationKind, UnitInterval};

struct Fixture {
    session: Box<DesktopSession>,
    photo: String,
    source: String,
    representation: String,
    _root: tempfile::TempDir,
}

impl Fixture {
    fn new() -> Self {
        let root = tempfile::tempdir().unwrap();
        let session = open_desktop_session(
            root.path().join("catalog.sqlite").to_str().unwrap(),
            root.path().join("cache").to_str().unwrap(),
        )
        .unwrap();
        let source = root
            .path()
            .join("synthetic.dng")
            .to_string_lossy()
            .into_owned();
        std::fs::write(&source, [0_u8; 16]).unwrap();
        let fingerprint = shadow_core::fingerprint_source(std::path::Path::new(&source)).unwrap();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: shadow_native_path::native_location(std::path::Path::new(&source)),
                byte_len: fingerprint.byte_len,
                modified_at_ms: fingerprint.modified_at_ms,
                now_ms: 100,
            })
            .unwrap();
        Self {
            session,
            photo: registered.photo_id.to_string(),
            source,
            representation: registered.representation_id.to_string(),
            _root: root,
        }
    }

    fn seed(&self) -> ffi::FfiPhotoEditState {
        self.session
            .autosave_grade_stack_working_at(
                &self.photo,
                &self.source,
                "",
                "",
                &GradeStackDraft::default(),
                100,
            )
            .unwrap()
    }

    fn publish(
        &self,
        state: &ffi::FfiPhotoEditState,
    ) -> AnyResult<ffi::FfiSharedGradePublishReceipt> {
        self.session.publish_shared_grade_node_from_photo(
            &self.photo,
            &self.source,
            &state.working_commit_id,
            &state.working_commit_id,
            &state.active_variant_id,
            &self.representation,
            &state.settings,
            &state.settings.grade_nodes[0].grade_node_id,
            "Shared look",
        )
    }

    fn subscribed_stack(
        &self,
        revision: &ffi::FfiSharedGradeNode,
        duplicate: bool,
    ) -> ffi::FfiPhotoEditState {
        let revision = self
            .session
            .catalog
            .shared_grade_revision_by_id(
                revision.layer_id.parse().unwrap(),
                revision.revision_id.parse().unwrap(),
            )
            .unwrap()
            .unwrap();
        let mut first = grade_node_draft_from_shared_revision(&revision).unwrap();
        first.recipe_v1_identity = GradeNodeRecipeV1Identity::new();
        first.shared.as_mut().unwrap().shared_follow_head = true;
        first.opacity = UnitInterval::new(0.37).unwrap();
        first.enabled = false;
        first.local_mask = Some(
            MaskDefinition::luminance_range(
                UnitInterval::ZERO,
                UnitInterval::ONE,
                UnitInterval::ZERO,
                true,
            )
            .unwrap(),
        );
        let mut stack = GradeStackDraft {
            grade_nodes: vec![GradeNodeDraft::neutral("Private"), first.clone()],
            ..GradeStackDraft::default()
        };
        if duplicate {
            let mut second = first;
            second.recipe_v1_identity = GradeNodeRecipeV1Identity::new();
            second.opacity = UnitInterval::new(0.63).unwrap();
            stack.grade_nodes.push(second);
        }
        let before = self
            .session
            .photo_edit_state(&self.photo, &self.source)
            .unwrap();
        self.session
            .autosave_grade_stack_working_at(
                &self.photo,
                &self.source,
                &before.working_commit_id,
                &before.working_commit_id,
                &stack,
                200,
            )
            .unwrap()
    }

    fn current(&self) -> ffi::FfiPhotoEditState {
        self.session
            .photo_edit_state(&self.photo, &self.source)
            .unwrap()
    }
}

fn next_revision(
    session: &DesktopSession,
    first: &ffi::FfiSharedGradeNode,
) -> ffi::FfiSharedGradeNode {
    let mut changed = first.grade_node.clone();
    changed.basic.exposure_stops = 0.8;
    session
        .publish_shared_grade_node("Shared look", &changed)
        .unwrap()
}

#[test]
fn source_self_link_and_shared_publication_are_atomic_and_preserve_instance_identity() {
    let fixture = Fixture::new();
    let before = fixture.seed();
    let receipt = fixture.publish(&before).unwrap();
    assert_eq!(receipt.shared_node.revision_number, 1);
    assert_eq!(
        receipt.state.settings.grade_nodes[0].grade_node_id,
        before.settings.grade_nodes[0].grade_node_id
    );
    assert!(receipt.state.settings.grade_nodes[0].shared_follow_head);
    assert_eq!(
        receipt.state.settings.grade_nodes[0].shared_revision_number,
        1
    );
    assert_ne!(receipt.state.working_commit_id, before.working_commit_id);
    assert_eq!(
        fixture.current().working_commit_id,
        receipt.state.working_commit_id
    );
    let mut edited = fixture.current();
    edited.settings.grade_nodes[0].basic.contrast_factor = 1.3;
    fixture
        .session
        .autosave_basic_edit_working_for_variant_at(
            &fixture.photo,
            &fixture.source,
            &edited.working_commit_id,
            &edited.working_commit_id,
            Some(&edited.active_variant_id),
            &edited.settings,
            200,
        )
        .unwrap();
    assert!(fixture.publish(&receipt.state).is_err());
    assert_eq!(
        fixture.session.shared_grade_nodes().unwrap()[0].revision_id,
        receipt.shared_node.revision_id
    );
    assert_eq!(
        fixture.session.shared_grade_nodes().unwrap()[0].revision_number,
        1
    );
}

#[test]
fn all_duplicate_instances_update_without_changing_masks_strength_or_private_nodes() {
    let source = Fixture::new();
    let source_before = source.seed();
    let first = source.publish(&source_before).unwrap().shared_node;
    // Replacing only source's working graph makes duplicate-instance behavior
    // independent from Library revision identity and canonical graph IDs.
    let before = source.subscribed_stack(&first, true);
    let second = next_revision(&source.session, &first);
    let target = source
        .session
        .shared_grade_linked_target(&first.layer_id, &source.photo, &source.source)
        .unwrap();
    assert_eq!(target.instances.len(), 2);
    let prepared = source
        .session
        .prepare_shared_grade_link_update(&second.layer_id, &second.revision_id, &target)
        .unwrap();
    assert_eq!(prepared.status, ffi::FfiSharedGradeUpdateStatus::Updated);
    assert_eq!(prepared.state.working_commit_id, before.working_commit_id);
    assert_eq!(
        prepared.state.settings.grade_nodes[1].basic.exposure_stops,
        0.0
    );
    let updated = source
        .session
        .commit_shared_grade_link_update(&second.layer_id, &second.revision_id, &target)
        .unwrap();
    assert_eq!(updated.status, ffi::FfiSharedGradeUpdateStatus::Updated);
    let old = decode_grade_stack_draft_recipe_v1(&before.settings).unwrap();
    let new = decode_grade_stack_draft_recipe_v1(&updated.state.settings).unwrap();
    assert_eq!(old.grade_nodes[0], new.grade_nodes[0]);
    for (old, new) in old.grade_nodes[1..].iter().zip(&new.grade_nodes[1..]) {
        assert_eq!(old.recipe_v1_identity, new.recipe_v1_identity);
        assert_eq!(old.opacity, new.opacity);
        assert_eq!(old.enabled, new.enabled);
        assert_eq!(old.local_mask, new.local_mask);
        assert_eq!(new.basic.exposure_stops, 0.8);
        assert_eq!(
            new.shared.unwrap().revision_id.to_string(),
            second.revision_id
        );
        assert!(new.shared.unwrap().shared_follow_head);
    }
    let historical = source
        .session
        .catalog
        .recipe_commit(
            source.photo.parse().unwrap(),
            before.working_commit_id.parse().unwrap(),
        )
        .unwrap()
        .unwrap();
    let historical = crate::recipe_v1::decode_grade_stack_draft_from_recipe_v1_snapshot(
        historical.commit.snapshot(),
    )
    .unwrap();
    assert_eq!(historical.grade_nodes[1].basic.exposure_stops, 0.0);
    assert_eq!(
        historical.grade_nodes[1]
            .shared
            .unwrap()
            .revision_id
            .to_string(),
        first.revision_id
    );
    let current_target = source
        .session
        .shared_grade_linked_target(&second.layer_id, &source.photo, &source.source)
        .unwrap();
    assert_eq!(
        source
            .session
            .prepare_shared_grade_link_update(
                &second.layer_id,
                &second.revision_id,
                &current_target
            )
            .unwrap()
            .status,
        ffi::FfiSharedGradeUpdateStatus::Unchanged
    );
}

#[test]
fn unpublished_local_adjustments_are_protected_and_follow_policy_survives() {
    let fixture = Fixture::new();
    let first = fixture.publish(&fixture.seed()).unwrap().shared_node;
    let mut before = fixture.subscribed_stack(&first, false);
    before.settings.grade_nodes[1].basic.exposure_stops = -0.4;
    let before = fixture
        .session
        .autosave_basic_edit_working_for_variant_at(
            &fixture.photo,
            &fixture.source,
            &before.working_commit_id,
            &before.working_commit_id,
            Some(&before.active_variant_id),
            &before.settings,
            200,
        )
        .unwrap();
    let second = next_revision(&fixture.session, &first);
    let target = fixture
        .session
        .shared_grade_linked_target(&first.layer_id, &fixture.photo, &fixture.source)
        .unwrap();
    let rejected = fixture
        .session
        .commit_shared_grade_link_update(&second.layer_id, &second.revision_id, &target)
        .unwrap();
    assert_eq!(
        rejected.status,
        ffi::FfiSharedGradeUpdateStatus::LocalChanged
    );
    assert_eq!(
        fixture.current().working_commit_id,
        before.working_commit_id
    );
    assert!(fixture.current().settings.grade_nodes[1].shared_follow_head);
    assert_eq!(
        fixture.current().settings.grade_nodes[1]
            .basic
            .exposure_stops,
        -0.4
    );
    assert_eq!(
        fixture.current().settings.grade_nodes[1].shared_revision_number,
        1
    );
}

#[test]
fn target_head_source_and_library_changes_return_typed_conflicts() {
    let fixture = Fixture::new();
    let first = fixture.publish(&fixture.seed()).unwrap().shared_node;
    let before = fixture.subscribed_stack(&first, false);
    let second = next_revision(&fixture.session, &first);
    let target = fixture
        .session
        .shared_grade_linked_target(&first.layer_id, &fixture.photo, &fixture.source)
        .unwrap();
    let mut source_changed = target.clone();
    source_changed.source_byte_len += 1;
    assert_eq!(
        fixture
            .session
            .prepare_shared_grade_link_update(
                &second.layer_id,
                &second.revision_id,
                &source_changed
            )
            .unwrap()
            .status,
        ffi::FfiSharedGradeUpdateStatus::Unavailable
    );
    assert_eq!(
        fixture
            .session
            .prepare_shared_grade_link_update(&first.layer_id, &first.revision_id, &target)
            .unwrap()
            .status,
        ffi::FfiSharedGradeUpdateStatus::HeadChanged
    );
    let mut settings = before.settings.clone();
    settings.grade_nodes[0].basic.exposure_stops = 0.5;
    let newer = fixture
        .session
        .autosave_basic_edit_working_for_variant_at(
            &fixture.photo,
            &fixture.source,
            &before.working_commit_id,
            &before.working_commit_id,
            Some(&before.active_variant_id),
            &settings,
            250,
        )
        .unwrap();
    assert_eq!(
        fixture
            .session
            .commit_shared_grade_link_update(&second.layer_id, &second.revision_id, &target)
            .unwrap()
            .status,
        ffi::FfiSharedGradeUpdateStatus::HeadChanged
    );
    assert_eq!(fixture.current().working_commit_id, newer.working_commit_id);
    assert!(
        fixture
            .session
            .publish_shared_grade_node("Stale", &first.grade_node)
            .is_err()
    );
}

#[test]
fn old_pinned_private_change_is_projected_after_head_moves_and_clears_on_reset() {
    let fixture = Fixture::new();
    let mut source = fixture.seed();
    source.settings.grade_nodes[0].basic.exposure_stops = 0.2;
    let first = fixture.publish(&source).unwrap().shared_node;
    let before = fixture.subscribed_stack(&first, false);
    // These instances have a private mask, strength and bypass state. None
    // changes the adjustment meaning compared with their own pinned revision.
    assert!(!before.settings.grade_nodes[1].shared_locally_modified);
    let mut local_settings = before.settings.clone();
    local_settings.grade_nodes[1].basic.exposure_stops = 0.35;
    let locally_saved = fixture
        .session
        .autosave_basic_edit_working_for_variant_at(
            &fixture.photo,
            &fixture.source,
            &before.working_commit_id,
            &before.working_commit_id,
            Some(&before.active_variant_id),
            &local_settings,
            250,
        )
        .unwrap();
    assert!(locally_saved.settings.grade_nodes[1].shared_locally_modified);
    let mut published = first.grade_node.clone();
    published.basic.exposure_stops = 0.55;
    let latest = fixture
        .session
        .publish_shared_grade_node("Shared look", &published)
        .unwrap();
    assert_eq!(latest.revision_number, 2);
    assert_ne!(latest.revision_id, first.revision_id);

    let reopened = open_desktop_session(
        fixture
            ._root
            .path()
            .join("catalog.sqlite")
            .to_str()
            .unwrap(),
        fixture._root.path().join("cache").to_str().unwrap(),
    )
    .unwrap();
    let loaded = reopened
        .photo_edit_state(&fixture.photo, &fixture.source)
        .unwrap();
    assert_eq!(loaded.working_commit_id, locally_saved.working_commit_id);
    assert_eq!(
        loaded.settings.grade_nodes[1].shared_revision_id,
        first.revision_id
    );
    assert_eq!(loaded.settings.grade_nodes[1].shared_revision_number, 1);
    assert!(loaded.settings.grade_nodes[1].shared_locally_modified);
    assert_eq!(loaded.settings.grade_nodes[1].basic.exposure_stops, 0.35);

    let copied = decode_grade_node_draft_recipe_v1(&loaded.settings.grade_nodes[1], 1)
        .unwrap()
        .duplicate();
    let copied = crate::recipe_v1::encode_grade_node_draft_recipe_v1(copied).unwrap();
    assert!(copied.shared_layer_id.is_empty());
    assert!(!copied.shared_follow_head);
    assert!(!copied.shared_locally_modified);

    let mut reset_settings = loaded.settings.clone();
    reset_settings.grade_nodes[1].basic.exposure_stops = 0.2;
    let reset = reopened
        .autosave_basic_edit_working_for_variant_at(
            &fixture.photo,
            &fixture.source,
            &loaded.working_commit_id,
            &loaded.working_commit_id,
            Some(&loaded.active_variant_id),
            &reset_settings,
            300,
        )
        .unwrap();
    assert!(!reset.settings.grade_nodes[1].shared_locally_modified);
    assert!(reset.settings.grade_nodes[1].shared_follow_head);
    assert_eq!(
        reset.settings.grade_nodes[1].shared_revision_id,
        first.revision_id
    );
    assert_eq!(reset.settings.grade_nodes[1].shared_revision_number, 1);
    let reset_loaded = reopened
        .photo_edit_state(&fixture.photo, &fixture.source)
        .unwrap();
    assert!(!reset_loaded.settings.grade_nodes[1].shared_locally_modified);
    assert_eq!(
        reopened.shared_grade_nodes().unwrap()[0].revision_id,
        latest.revision_id
    );
}
