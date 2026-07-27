use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    CatalogHandle, CommitEditRepository, EditObjectPackWrite, EditRepositoryRefUpdate,
};
use shadow_domain::{
    EditEntityMapV1, EditObject, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId,
    LayerId, LayerRevision, LayerRevisionId, LibraryRootV1,
};

use crate::session_edit_history::LIBRARY_EDIT_MAIN_REF;

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
    let head = load_library_head(catalog)?;
    let object_id = head
        .shared_heads
        .get(&layer_id.to_string())
        .ok_or_else(|| anyhow!("shared Grade Node {layer_id} is unavailable"))?;
    let revision = load_revision(catalog, object_id)?;
    if revision.layer_id() != layer_id {
        bail!("shared Grade Node head points to a different stable layer");
    }
    Ok(revision)
}

pub(crate) fn publish_shared_grade_revision(
    catalog: &CatalogHandle,
    layer_id: LayerId,
    label: &str,
    graph: shadow_domain::EditGraph,
    created_at_ms: i64,
) -> AnyResult<LayerRevision> {
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
    let revision = LayerRevision::new(
        LayerRevisionId::new_v7(),
        layer_id,
        previous
            .as_ref()
            .map_or(1, |revision| revision.revision_number() + 1),
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
    catalog.store_edit_object_pack(&EditObjectPackWrite {
        objects: vec![root_pack, shared_heads_pack, revision_pack],
        created_at_ms,
    })?;
    catalog.commit_edit_repository(&CommitEditRepository {
        commit: repository_commit,
        update_refs: vec![EditRepositoryRefUpdate {
            name: LIBRARY_EDIT_MAIN_REF.into(),
            kind: EditRepositoryRefKind::Branch,
            expected: expectation,
            updated_at_ms: created_at_ms,
        }],
    })?;
    Ok(revision)
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
