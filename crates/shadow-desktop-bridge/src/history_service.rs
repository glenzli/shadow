//! Read-only projection of durable photo and Library edit history.
//!
//! Catalog owns immutable storage and keyset pagination. This service decorates
//! those records with desktop-facing ref identities and bounded semantic diffs;
//! it deliberately owns no editor or QML state.

use std::collections::HashMap;

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_catalog::{
    CatalogHandle, EditRepositoryCommitRecord, EditRepositoryHistoryCursor,
    EditRepositoryRefRecord, RecipeCommitRecord, RecipeHistoryCursor, RecipeRefKind,
    RecipeRefRecord,
};
use shadow_domain::{
    EditCommitId, EditEntityMapV1, EditObjectId, EditRepositoryRefKind, LibraryRootV1, PhotoId,
};

use crate::{
    edit_version_diff::edit_version_diff, ffi, session_edit_history::LIBRARY_EDIT_MAIN_REF,
};

#[derive(Debug)]
pub(crate) struct HistoryService {
    catalog: CatalogHandle,
}

impl HistoryService {
    pub(crate) fn new(catalog: CatalogHandle) -> Self {
        Self { catalog }
    }

    pub(crate) fn photo_page(
        &self,
        photo_id: &str,
        after: &ffi::FfiHistoryCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiPhotoHistoryPage> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse photo history owner {photo_id}"))?;
        let cursor = recipe_cursor(after)?;
        let page = self.catalog.recipe_history_page(
            photo_id,
            cursor.as_ref(),
            usize::try_from(limit).context("convert photo history page limit")?,
        )?;
        let mut comparison_records = page
            .entries
            .iter()
            .map(|entry| entry.record.clone())
            .collect::<Vec<_>>();
        for entry in &page.entries {
            let Some(parent_id) = entry.record.commit.parents().first().copied() else {
                continue;
            };
            if comparison_records
                .iter()
                .any(|record| record.commit.id() == parent_id)
            {
                continue;
            }
            let parent = self
                .catalog
                .recipe_commit(photo_id, parent_id)?
                .ok_or_else(|| {
                    anyhow!(
                        "photo history commit {} references missing parent {parent_id}",
                        entry.record.commit.id()
                    )
                })?;
            comparison_records.push(parent);
        }
        let entries = page
            .entries
            .iter()
            .map(|entry| photo_entry(&entry.record, &entry.refs, &comparison_records))
            .collect::<AnyResult<Vec<_>>>()?;
        Ok(ffi::FfiPhotoHistoryPage {
            entries,
            has_more: page.next_cursor.is_some(),
            next_cursor: page.next_cursor.map(recipe_ffi_cursor).unwrap_or_default(),
        })
    }

    pub(crate) fn library_page(
        &self,
        after: &ffi::FfiHistoryCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryHistoryPage> {
        let cursor = library_cursor(after)?;
        let page = self.catalog.edit_repository_history_page(
            cursor.as_ref(),
            usize::try_from(limit).context("convert Library history page limit")?,
        )?;
        let mut comparison_records = page
            .entries
            .iter()
            .map(|entry| (entry.record.commit.id(), entry.record.clone()))
            .collect::<HashMap<_, _>>();
        for entry in &page.entries {
            let Some(parent_id) = entry.record.commit.payload().parents.first().copied() else {
                continue;
            };
            if comparison_records.contains_key(&parent_id) {
                continue;
            }
            let parent = self
                .catalog
                .edit_repository_commit(parent_id)?
                .ok_or_else(|| anyhow!("Library history references missing parent {parent_id}"))?;
            comparison_records.insert(parent_id, parent);
        }
        let mut root_cache = HashMap::new();
        let mut entity_map_cache = HashMap::new();
        let entries = page
            .entries
            .iter()
            .map(|entry| {
                let diff = self.library_diff(
                    entry.record.commit.payload().root,
                    entry.record.commit.payload().parents.first().copied(),
                    &comparison_records,
                    &mut root_cache,
                    &mut entity_map_cache,
                )?;
                Ok(ffi::FfiLibraryHistoryEntry {
                    commit_id: entry.record.commit.id().to_string(),
                    message: entry
                        .record
                        .commit
                        .payload()
                        .message
                        .clone()
                        .unwrap_or_default(),
                    created_at_ms: entry.record.commit.payload().created_at_ms,
                    parent_commit_ids: entry
                        .record
                        .commit
                        .payload()
                        .parents
                        .iter()
                        .map(ToString::to_string)
                        .collect(),
                    refs: entry.refs.iter().map(library_ref).collect(),
                    is_root: entry.record.commit.payload().parents.is_empty(),
                    is_head: entry
                        .refs
                        .iter()
                        .any(|reference| reference.name == LIBRARY_EDIT_MAIN_REF),
                    photo_changes: diff.photos,
                    shared_grade_changes: diff.shared_grades,
                    mask_changes: diff.masks,
                    style_changes: diff.styles,
                    output_state_changes: diff.output_states,
                })
            })
            .collect::<AnyResult<Vec<_>>>()?;
        Ok(ffi::FfiLibraryHistoryPage {
            entries,
            has_more: page.next_cursor.is_some(),
            next_cursor: page.next_cursor.map(library_ffi_cursor).unwrap_or_default(),
        })
    }

    pub(crate) fn library_ref_page(
        &self,
        after_name: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryHistoryRefPage> {
        let page = self.catalog.edit_repository_ref_page(
            (!after_name.is_empty()).then_some(after_name),
            usize::try_from(limit).context("convert Library history ref page limit")?,
        )?;
        Ok(ffi::FfiLibraryHistoryRefPage {
            refs: page.refs.iter().map(library_ref).collect(),
            has_more: page.next_cursor.is_some(),
            next_cursor: page.next_cursor.unwrap_or_default(),
        })
    }

    fn library_diff(
        &self,
        after_root_id: EditObjectId,
        parent_id: Option<EditCommitId>,
        comparison_records: &HashMap<EditCommitId, EditRepositoryCommitRecord>,
        root_cache: &mut HashMap<EditObjectId, LibraryRootV1>,
        entity_map_cache: &mut HashMap<EditObjectId, EditEntityMapV1>,
    ) -> AnyResult<LibraryDiff> {
        let Some(parent_id) = parent_id else {
            return Ok(LibraryDiff::default());
        };
        let parent = comparison_records
            .get(&parent_id)
            .ok_or_else(|| anyhow!("Library history references missing parent {parent_id}"))?;
        let before = self.library_root(parent.commit.payload().root, root_cache)?;
        let after = self.library_root(after_root_id, root_cache)?;
        Ok(LibraryDiff {
            photos: self.entity_change_count(
                before.photo_recipes,
                after.photo_recipes,
                entity_map_cache,
            )?,
            shared_grades: self.entity_change_count(
                before.shared_grade_heads,
                after.shared_grade_heads,
                entity_map_cache,
            )?,
            masks: self.entity_change_count(before.masks, after.masks, entity_map_cache)?,
            styles: self.entity_change_count(before.styles, after.styles, entity_map_cache)?,
            output_states: self.entity_change_count(
                before.output_states,
                after.output_states,
                entity_map_cache,
            )?,
        })
    }

    fn library_root(
        &self,
        id: EditObjectId,
        cache: &mut HashMap<EditObjectId, LibraryRootV1>,
    ) -> AnyResult<LibraryRootV1> {
        if let Some(root) = cache.get(&id) {
            return Ok(*root);
        }
        let record = self
            .catalog
            .edit_object(id)?
            .ok_or_else(|| anyhow!("Library history root object {id} is missing"))?;
        let root = LibraryRootV1::from_object(&record.object)
            .with_context(|| format!("decode Library history root object {id}"))?;
        cache.insert(id, root);
        Ok(root)
    }

    fn entity_change_count(
        &self,
        before: Option<EditObjectId>,
        after: Option<EditObjectId>,
        cache: &mut HashMap<EditObjectId, EditEntityMapV1>,
    ) -> AnyResult<u32> {
        if before == after {
            return Ok(0);
        }
        self.ensure_entity_map(before, cache)?;
        self.ensure_entity_map(after, cache)?;
        let empty = EditEntityMapV1::new(Vec::new()).context("create empty Library entity map")?;
        let before_map = before.and_then(|id| cache.get(&id)).unwrap_or(&empty);
        let after_map = after.and_then(|id| cache.get(&id)).unwrap_or(&empty);
        u32::try_from(before_map.diff(after_map).len())
            .context("Library history diff exceeds desktop ABI")
    }

    fn ensure_entity_map(
        &self,
        id: Option<EditObjectId>,
        cache: &mut HashMap<EditObjectId, EditEntityMapV1>,
    ) -> AnyResult<()> {
        let Some(id) = id else {
            return Ok(());
        };
        if cache.contains_key(&id) {
            return Ok(());
        }
        let record = self
            .catalog
            .edit_object(id)?
            .ok_or_else(|| anyhow!("Library history entity-map object {id} is missing"))?;
        let map = EditEntityMapV1::from_object(&record.object)
            .with_context(|| format!("decode Library history entity-map object {id}"))?;
        cache.insert(id, map);
        Ok(())
    }
}

#[derive(Debug, Default)]
struct LibraryDiff {
    photos: u32,
    shared_grades: u32,
    masks: u32,
    styles: u32,
    output_states: u32,
}

fn photo_entry(
    record: &RecipeCommitRecord,
    refs: &[RecipeRefRecord],
    comparison_records: &[RecipeCommitRecord],
) -> AnyResult<ffi::FfiPhotoHistoryEntry> {
    let diff = edit_version_diff(record, comparison_records)?;
    Ok(ffi::FfiPhotoHistoryEntry {
        commit_id: record.commit.id().to_string(),
        name: record.commit.message().unwrap_or_default().to_owned(),
        created_at_ms: record.commit.created_at_ms(),
        parent_commit_ids: record
            .commit
            .parents()
            .iter()
            .map(ToString::to_string)
            .collect(),
        refs: refs.iter().map(recipe_ref).collect(),
        is_named: refs
            .iter()
            .any(|reference| reference.kind == RecipeRefKind::NamedVersion),
        is_working: refs
            .iter()
            .any(|reference| reference.kind == RecipeRefKind::Working),
        is_root: diff.is_root,
        recipe_schema_changed: diff.recipe_schema_changed,
        grade_nodes_added: diff.grade_nodes_added,
        grade_nodes_removed: diff.grade_nodes_removed,
        grade_nodes_moved: diff.grade_nodes_moved,
        grade_nodes_modified: diff.grade_nodes_modified,
        render_ops_added: diff.render_ops_added,
        render_ops_removed: diff.render_ops_removed,
        render_ops_modified: diff.render_ops_modified,
        render_op_parameter_blocks_changed: diff.render_op_parameter_blocks_changed,
        changed_parameter_keys: diff.changed_basic_parameters,
        has_other_changes: diff.has_other_changes,
    })
}

fn recipe_cursor(cursor: &ffi::FfiHistoryCursor) -> AnyResult<Option<RecipeHistoryCursor>> {
    parse_cursor(cursor, |commit_id| {
        Ok(RecipeHistoryCursor {
            created_at_ms: cursor.created_at_ms,
            commit_id: commit_id
                .parse()
                .context("parse Recipe history cursor id")?,
        })
    })
}

fn library_cursor(
    cursor: &ffi::FfiHistoryCursor,
) -> AnyResult<Option<EditRepositoryHistoryCursor>> {
    parse_cursor(cursor, |commit_id| {
        Ok(EditRepositoryHistoryCursor {
            created_at_ms: cursor.created_at_ms,
            commit_id: commit_id
                .parse()
                .context("parse Library history cursor id")?,
        })
    })
}

fn parse_cursor<T>(
    cursor: &ffi::FfiHistoryCursor,
    parse: impl FnOnce(&str) -> AnyResult<T>,
) -> AnyResult<Option<T>> {
    if cursor.commit_id.is_empty() {
        if cursor.created_at_ms == 0 {
            return Ok(None);
        }
        return Err(anyhow!(
            "history cursor timestamp requires a non-empty commit id"
        ));
    }
    parse(&cursor.commit_id).map(Some)
}

fn recipe_ffi_cursor(cursor: RecipeHistoryCursor) -> ffi::FfiHistoryCursor {
    ffi::FfiHistoryCursor {
        created_at_ms: cursor.created_at_ms,
        commit_id: cursor.commit_id.to_string(),
    }
}

fn library_ffi_cursor(cursor: EditRepositoryHistoryCursor) -> ffi::FfiHistoryCursor {
    ffi::FfiHistoryCursor {
        created_at_ms: cursor.created_at_ms,
        commit_id: cursor.commit_id.to_string(),
    }
}

fn recipe_ref(reference: &RecipeRefRecord) -> ffi::FfiHistoryRef {
    ffi::FfiHistoryRef {
        name: reference.name.clone(),
        kind: match reference.kind {
            RecipeRefKind::Working => ffi::FfiHistoryRefKind::Working,
            RecipeRefKind::Branch => ffi::FfiHistoryRefKind::Branch,
            RecipeRefKind::NamedVersion => ffi::FfiHistoryRefKind::NamedVersion,
            RecipeRefKind::Tag => ffi::FfiHistoryRefKind::Tag,
        },
        commit_id: reference.commit_id.to_string(),
        updated_at_ms: reference.updated_at_ms,
    }
}

fn library_ref(reference: &EditRepositoryRefRecord) -> ffi::FfiHistoryRef {
    ffi::FfiHistoryRef {
        name: reference.name.clone(),
        kind: match reference.kind {
            EditRepositoryRefKind::Branch => ffi::FfiHistoryRefKind::Branch,
            EditRepositoryRefKind::NamedVersion => ffi::FfiHistoryRefKind::NamedVersion,
            EditRepositoryRefKind::Tag => ffi::FfiHistoryRefKind::Tag,
        },
        commit_id: reference.commit_id.to_string(),
        updated_at_ms: reference.updated_at_ms,
    }
}

#[cfg(test)]
mod tests;
