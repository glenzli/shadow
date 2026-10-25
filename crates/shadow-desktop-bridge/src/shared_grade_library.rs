use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    CatalogHandle, CommitEditRepository, EditObjectPackWrite, EditRepositoryRefUpdate,
};
use shadow_domain::{
    EditEntityMapV1, EditObject, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId,
    LayerId, LayerRevision, LayerRevisionId, LibraryRootV1,
};

use crate::{
    recipe_v1::{
        GradeNodeDraft, GradeNodeRecipeV1Identity, encode_grade_node_as_recipe_v1_layer,
        recipe_color_grading_render_op_id, recipe_finishing_effects_render_op_id,
        recipe_v1_derived_render_op_id, recipe_v1_lut_render_op_id,
        recipe_v1_oklab_color_warper_render_op_id,
        recipe_v1_oklab_lightness_tone_curve_render_op_id, recipe_v1_perceptual_color_render_op_id,
        recipe_v1_rgb_tone_curves_render_op_id, recipe_v1_selective_tone_render_op_id,
        recipe_v1_sharpen_render_op_id,
    },
    session_edit_history::LIBRARY_EDIT_MAIN_REF,
};

pub(crate) struct PreparedSharedGradePublication {
    pub(crate) revision: LayerRevision,
    pub(crate) objects: EditObjectPackWrite,
    pub(crate) repository: CommitEditRepository,
}

/// A Library graph has canonical shared identities, independent of whichever
/// photo instance publishes it. Instance masks and strength never enter it.
pub(crate) fn canonical_shared_graph(
    draft: &GradeNodeDraft,
    layer_id: LayerId,
) -> AnyResult<shadow_domain::EditGraph> {
    let mut canonical = draft.clone();
    let grade_node_id = shadow_domain::LayerInstanceId::from_uuid(layer_id.as_uuid());
    canonical.recipe_v1_identity = GradeNodeRecipeV1Identity {
        grade_node_id,
        exposure_render_op_id: recipe_v1_derived_render_op_id(
            b"shadow.desktop.shared-exposure-slot-id.v1\0",
            grade_node_id,
        ),
        contrast_render_op_id: recipe_v1_derived_render_op_id(
            b"shadow.desktop.shared-contrast-slot-id.v1\0",
            grade_node_id,
        ),
        white_balance_render_op_id: recipe_v1_derived_render_op_id(
            b"shadow.desktop.shared-white-balance-slot-id.v1\0",
            grade_node_id,
        ),
        saturation_render_op_id: recipe_v1_derived_render_op_id(
            b"shadow.desktop.shared-saturation-slot-id.v1\0",
            grade_node_id,
        ),
        rgb_tone_curves_render_op_id: recipe_v1_rgb_tone_curves_render_op_id(grade_node_id),
        oklab_lightness_curve_render_op_id: recipe_v1_oklab_lightness_tone_curve_render_op_id(
            grade_node_id,
        ),
        selective_tone_render_op_id: recipe_v1_selective_tone_render_op_id(grade_node_id),
        perceptual_color_render_op_id: recipe_v1_perceptual_color_render_op_id(grade_node_id),
        oklab_color_warper_render_op_id: recipe_v1_oklab_color_warper_render_op_id(grade_node_id),
        lut_render_op_id: recipe_v1_lut_render_op_id(grade_node_id),
        color_grading_render_op_id: recipe_color_grading_render_op_id(grade_node_id),
        sharpen_render_op_id: recipe_v1_sharpen_render_op_id(grade_node_id),
        finishing_effects_render_op_id: recipe_finishing_effects_render_op_id(grade_node_id),
    };
    canonical.shared = None;
    canonical.local_mask = None;
    canonical.composite_mask = None;
    canonical.preserved_managed_raster = None;
    Ok(encode_grade_node_as_recipe_v1_layer(&canonical)?
        .content()
        .graph()
        .clone())
}

struct LibraryHead {
    parent: Option<shadow_domain::EditCommitId>,
    root: LibraryRootV1,
    shared_heads: EditEntityMapV1,
}

pub(crate) fn shared_grade_revisions(catalog: &CatalogHandle) -> AnyResult<Vec<LayerRevision>> {
    let head = load_library_head(catalog)?;
    let mut revisions = head
        .shared_heads
        .entries()
        .iter()
        .map(|entry| load_revision(catalog, entry.value))
        .collect::<AnyResult<Vec<_>>>()?;
    revisions.sort_by(|left, right| {
        left.label()
            .to_lowercase()
            .cmp(&right.label().to_lowercase())
            .then_with(|| left.layer_id().cmp(&right.layer_id()))
    });
    Ok(revisions)
}

pub(crate) fn shared_grade_revision(
    catalog: &CatalogHandle,
    layer_id: LayerId,
) -> AnyResult<LayerRevision> {
    shared_grade_revision_head(catalog, layer_id).map(|(_, revision)| revision)
}

pub(crate) fn shared_grade_revision_head(
    catalog: &CatalogHandle,
    layer_id: LayerId,
) -> AnyResult<(shadow_domain::EditCommitId, LayerRevision)> {
    try_shared_grade_revision_head(catalog, layer_id)?
        .ok_or_else(|| anyhow!("shared Grade Node {layer_id} is unavailable"))
}

pub(crate) fn try_shared_grade_revision_head(
    catalog: &CatalogHandle,
    layer_id: LayerId,
) -> AnyResult<Option<(shadow_domain::EditCommitId, LayerRevision)>> {
    let head = load_library_head(catalog)?;
    let Some(object_id) = head.shared_heads.get(&layer_id.to_string()) else {
        return Ok(None);
    };
    let revision = load_revision(catalog, object_id)?;
    if revision.layer_id() != layer_id {
        bail!("shared Grade Node head points to a different stable layer");
    }
    Ok(Some((
        head.parent
            .ok_or_else(|| anyhow!("shared Grade Node has no Library commit"))?,
        revision,
    )))
}

pub(crate) fn publish_shared_grade_revision(
    catalog: &CatalogHandle,
    layer_id: LayerId,
    label: &str,
    graph: shadow_domain::EditGraph,
    expected_revision_id: Option<LayerRevisionId>,
    created_at_ms: i64,
) -> AnyResult<LayerRevision> {
    let prepared = prepare_shared_grade_revision(
        catalog,
        layer_id,
        label,
        graph,
        expected_revision_id,
        created_at_ms,
    )?;
    catalog.store_edit_object_pack(&prepared.objects)?;
    catalog.commit_edit_repository(&prepared.repository)?;
    Ok(prepared.revision)
}

pub(crate) fn prepare_shared_grade_revision(
    catalog: &CatalogHandle,
    layer_id: LayerId,
    label: &str,
    graph: shadow_domain::EditGraph,
    expected_revision_id: Option<LayerRevisionId>,
    created_at_ms: i64,
) -> AnyResult<PreparedSharedGradePublication> {
    let head = load_library_head(catalog)?;
    let key = layer_id.to_string();
    let previous = head
        .shared_heads
        .get(&key)
        .map(|object_id| load_revision(catalog, object_id))
        .transpose()?;
    if previous
        .as_ref()
        .is_some_and(|revision| revision.layer_id() != layer_id)
    {
        bail!("shared Grade Node head points to a different stable layer");
    }
    if previous.as_ref().map(LayerRevision::id) != expected_revision_id {
        bail!("shared Grade Node Library revision changed before publication");
    }
    let revision = LayerRevision::new(
        LayerRevisionId::new_v7(),
        layer_id,
        previous.as_ref().map_or(Ok(1), |revision| {
            revision
                .revision_number()
                .checked_add(1)
                .ok_or_else(|| anyhow!("shared Grade Node revision number exhausted"))
        })?,
        previous.as_ref().map(LayerRevision::id),
        label,
        graph,
    )?;
    let revision_object =
        EditObject::from_canonical_json(EditObjectKind::GradeNodeRevision, 1, &revision)?;
    let revision_pack = EditObjectPack::new(revision_object, Vec::new())?;
    let shared_heads = head
        .shared_heads
        .with_entry(key, revision_pack.object().id())?;
    let shared_heads_pack = shared_heads.into_object_pack()?;
    let root_pack = LibraryRootV1 {
        photo_recipes: head.root.photo_recipes,
        shared_grade_heads: Some(shared_heads_pack.object().id()),
        masks: head.root.masks,
        styles: head.root.styles,
        output_states: head.root.output_states,
    }
    .into_object_pack()?;
    let repository_commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root: root_pack.object().id(),
        parents: head.parent.into_iter().collect(),
        message: Some(format!("Shared Grade Node · {label}")),
        created_at_ms,
    })?;
    let expectation = head.parent.map_or(
        EditRepositoryRefExpectation::Missing,
        EditRepositoryRefExpectation::At,
    );
    Ok(PreparedSharedGradePublication {
        revision,
        objects: EditObjectPackWrite {
            objects: vec![root_pack, shared_heads_pack, revision_pack],
            created_at_ms,
        },
        repository: CommitEditRepository {
            commit: repository_commit,
            update_refs: vec![EditRepositoryRefUpdate {
                name: LIBRARY_EDIT_MAIN_REF.into(),
                kind: EditRepositoryRefKind::Branch,
                expected: expectation,
                updated_at_ms: created_at_ms,
            }],
        },
    })
}

fn load_library_head(catalog: &CatalogHandle) -> AnyResult<LibraryHead> {
    let Some(head) = catalog.edit_repository_ref(LIBRARY_EDIT_MAIN_REF)? else {
        return Ok(LibraryHead {
            parent: None,
            root: empty_root(),
            shared_heads: EditEntityMapV1::new(Vec::new())?,
        });
    };
    if head.kind != EditRepositoryRefKind::Branch {
        bail!("Library edit ref {LIBRARY_EDIT_MAIN_REF} is not a branch");
    }
    let commit = catalog
        .edit_repository_commit(head.commit_id)?
        .ok_or_else(|| anyhow!("Library edit head commit {} is missing", head.commit_id))?;
    let root_record = catalog
        .edit_object(commit.commit.payload().root)?
        .ok_or_else(|| {
            anyhow!(
                "Library edit root {} is missing",
                commit.commit.payload().root
            )
        })?;
    let root = LibraryRootV1::from_object(&root_record.object)
        .context("decode Library edit root for shared Grade Nodes")?;
    let shared_heads = match root.shared_grade_heads {
        Some(map_id) => {
            let map_record = catalog
                .edit_object(map_id)?
                .ok_or_else(|| anyhow!("shared Grade Node head map {map_id} is missing"))?;
            EditEntityMapV1::from_object(&map_record.object)
                .context("decode shared Grade Node head map")?
        }
        None => EditEntityMapV1::new(Vec::new())?,
    };
    Ok(LibraryHead {
        parent: Some(head.commit_id),
        root,
        shared_heads,
    })
}

fn load_revision(
    catalog: &CatalogHandle,
    object_id: shadow_domain::EditObjectId,
) -> AnyResult<LayerRevision> {
    let record = catalog
        .edit_object(object_id)?
        .ok_or_else(|| anyhow!("shared Grade Node revision {object_id} is missing"))?;
    if record.object.kind() != EditObjectKind::GradeNodeRevision
        || record.object.format_version() != 1
    {
        bail!("shared Grade Node head {object_id} has the wrong object type");
    }
    let revision = record.object.decode::<LayerRevision>()?;
    revision.validate()?;
    Ok(revision)
}

const fn empty_root() -> LibraryRootV1 {
    LibraryRootV1 {
        photo_recipes: None,
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
}

#[cfg(test)]
mod tests;
