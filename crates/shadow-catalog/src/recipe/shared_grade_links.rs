//! Bounded current-working subscription reads and guarded shared-grade writes.
//!
//! A subscription is policy on a materialized Recipe layer. Immutable commits
//! remain executable without reading a mutable Library head.

use rusqlite::{OptionalExtension, Transaction, TransactionBehavior, params};
use shadow_domain::{
    EditCommitId, EditObjectKind, EntityId, LayerContent, LayerId, LayerInstanceId, LayerRevision,
    LayerRevisionId, LayerRevisionSelector, PhotoId, PhotoVariantId, RecipeCommitId,
    RepresentationId,
};

use super::{CommitRecipe, RecipeCommitRecord, commit_recipe_in_transaction};
use crate::{
    Catalog, CatalogError, CommitRecipeAndEditRepository, CommitRecipeAndEditRepositoryResult,
    RepresentationFingerprint, row_codec::read_id,
};

pub const MAX_SHARED_GRADE_LINK_PAGE_SIZE: usize = 256;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct SharedGradeLinkedInstanceRecord {
    pub instance_id: LayerInstanceId,
    pub revision_id: LayerRevisionId,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SharedGradeLinkedWorkingRecord {
    pub photo_id: PhotoId,
    pub variant_id: PhotoVariantId,
    pub commit_id: RecipeCommitId,
    pub instances: Vec<SharedGradeLinkedInstanceRecord>,
}

/// A page bounds inspected current Recipes, rather than accumulating all
/// matching photos. Empty pages may still have a continuation.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SharedGradeLinkedWorkingPage {
    pub records: Vec<SharedGradeLinkedWorkingRecord>,
    pub next_photo_id: Option<PhotoId>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SharedGradeSourceGuard {
    pub representation_id: RepresentationId,
    pub source_path: String,
    pub fingerprint: RepresentationFingerprint,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CommitSharedGradeLinkUpdate {
    pub recipe: CommitRecipe,
    pub expected_variant_id: PhotoVariantId,
    pub expected_library_commit_id: EditCommitId,
    pub library_ref_name: String,
    pub source: SharedGradeSourceGuard,
}

#[derive(Debug, Clone, PartialEq)]
pub enum SharedGradeLinkWriteResult {
    Committed(RecipeCommitRecord),
    HeadChanged,
    SourceChanged,
}

impl Catalog {
    pub fn shared_grade_linked_working_page(
        &self,
        layer_id: LayerId,
        current_revision_id: LayerRevisionId,
        after_photo_id: Option<PhotoId>,
        limit: usize,
    ) -> Result<SharedGradeLinkedWorkingPage, CatalogError> {
        if !(1..=MAX_SHARED_GRADE_LINK_PAGE_SIZE).contains(&limit) {
            return Err(CatalogError::InvalidRecipe(format!(
                "shared Grade Node page limit {limit} is outside 1 through {MAX_SHARED_GRADE_LINK_PAGE_SIZE}"
            )));
        }
        let mut statement = self.connection.prepare(
            "SELECT s.photo_id, s.active_variant_id, rr.commit_id
             FROM photo_variant_state s
             JOIN photo_variants v ON v.id = s.active_variant_id AND v.photo_id = s.photo_id
             JOIN recipe_refs rr ON rr.photo_id = s.photo_id AND rr.name = 'working'
             JOIN photos p ON p.id = s.photo_id AND p.lifecycle_state = 'active'
             WHERE s.photo_id > ?1
               AND rr.kind = 'working' AND v.head_commit_id = rr.commit_id
             ORDER BY s.photo_id
             LIMIT ?2",
        )?;
        let mut rows = statement
            .query_map(
                params![
                    after_photo_id
                        .map(|id| id.as_bytes().to_vec())
                        .unwrap_or_default(),
                    i64::try_from(limit + 1).expect("bounded shared-grade page size"),
                ],
                |row| {
                    Ok((
                        read_id::<PhotoId>(row, 0)?,
                        read_id::<PhotoVariantId>(row, 1)?,
                        read_id::<RecipeCommitId>(row, 2)?,
                    ))
                },
            )?
            .collect::<Result<Vec<_>, _>>()?;
        let has_more = rows.len() > limit;
        if has_more {
            rows.pop();
        }
        let next_photo_id = has_more.then(|| rows.last().expect("nonempty page").0);
        let mut records = Vec::new();
        for (photo_id, variant_id, commit_id) in rows {
            let record = self
                .recipe_commit(photo_id, commit_id)?
                .ok_or(CatalogError::RecipeCommitNotFound(commit_id))?;
            let instances = record
                .commit
                .snapshot()
                .layers()
                .iter()
                .filter_map(|layer| match layer.content() {
                    LayerContent::Shared {
                        layer_id: candidate,
                        revision: LayerRevisionSelector::Pinned(revision_id),
                        ..
                    } if *candidate == layer_id && layer.shared_follow_head() => {
                        Some(SharedGradeLinkedInstanceRecord {
                            instance_id: layer.id(),
                            revision_id: *revision_id,
                        })
                    }
                    _ => None,
                })
                .collect::<Vec<_>>();
            if instances
                .iter()
                .any(|instance| instance.revision_id != current_revision_id)
            {
                records.push(SharedGradeLinkedWorkingRecord {
                    photo_id,
                    variant_id,
                    commit_id,
                    instances,
                });
            }
        }
        Ok(SharedGradeLinkedWorkingPage {
            records,
            next_photo_id,
        })
    }

    /// Resolves a stable revision identity among immutable shared-grade
    /// objects, including revisions that no longer occupy the Library head.
    pub fn shared_grade_revision_by_id(
        &self,
        layer_id: LayerId,
        revision_id: LayerRevisionId,
    ) -> Result<Option<LayerRevision>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT digest FROM edit_objects
             WHERE kind = ?1 AND format_version = 1
               AND json_extract(payload, '$.id') = ?2
               AND json_extract(payload, '$.layer_id') = ?3
             LIMIT 2",
        )?;
        let ids = statement
            .query_map(
                params![
                    EditObjectKind::GradeNodeRevision.as_str(),
                    revision_id.to_string(),
                    layer_id.to_string(),
                ],
                |row| crate::cache_artifact::digest(row.get(0)?, 0),
            )?
            .collect::<Result<Vec<_>, _>>()?;
        if ids.len() > 1 {
            return Err(CatalogError::InvalidEditObject(format!(
                "shared Grade Node revision {revision_id} has conflicting immutable objects"
            )));
        }
        ids.first()
            .map(|digest| {
                let id = shadow_domain::EditObjectId::from_bytes(*digest);
                let object = self
                    .edit_object(id)?
                    .ok_or(CatalogError::EditObjectNotFound(id))?;
                let revision: LayerRevision = object
                    .object
                    .decode()
                    .map_err(|error| CatalogError::InvalidEditObject(error.to_string()))?;
                revision
                    .validate()
                    .map_err(|error| CatalogError::InvalidEditObject(error.to_string()))?;
                Ok(revision)
            })
            .transpose()
    }

    /// Checks source, active Variant, working head and captured Library head
    /// under the same writer reservation as the new immutable Recipe.
    pub fn commit_shared_grade_link_update(
        &mut self,
        request: &CommitSharedGradeLinkUpdate,
    ) -> Result<SharedGradeLinkWriteResult, CatalogError> {
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if !source_guard_matches(&transaction, request.recipe.photo_id, &request.source)? {
            return Ok(SharedGradeLinkWriteResult::SourceChanged);
        }
        let head = transaction
            .query_row(
                "SELECT commit_id FROM edit_repository_refs WHERE name = ?1 AND kind = 'branch'",
                [&request.library_ref_name],
                |row| crate::cache_artifact::digest(row.get(0)?, 0),
            )
            .optional()?;
        if head.as_ref().map(|bytes| bytes.as_slice())
            != Some(request.expected_library_commit_id.as_bytes().as_slice())
        {
            return Ok(SharedGradeLinkWriteResult::HeadChanged);
        }
        match super::variants::ensure_active_variant(
            &transaction,
            request.recipe.photo_id,
            request.expected_variant_id,
        ) {
            Err(CatalogError::PhotoVariantExpectationMismatch { .. }) => {
                return Ok(SharedGradeLinkWriteResult::HeadChanged);
            }
            result => result?,
        }
        let record = match commit_recipe_in_transaction(&transaction, &request.recipe) {
            Err(CatalogError::RecipeRefExpectationMismatch { .. }) => {
                return Ok(SharedGradeLinkWriteResult::HeadChanged);
            }
            result => result?,
        };
        transaction.commit()?;
        Ok(SharedGradeLinkWriteResult::Committed(record))
    }

    /// Source self-link and Library publication share one transaction. A
    /// source drift leaves both commits and every ref unchanged.
    pub fn commit_shared_grade_publication(
        &mut self,
        request: &CommitRecipeAndEditRepository,
        source: &SharedGradeSourceGuard,
    ) -> Result<Option<CommitRecipeAndEditRepositoryResult>, CatalogError> {
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if !source_guard_matches(&transaction, request.recipe.photo_id, source)? {
            return Ok(None);
        }
        if let Some(expected) = request.expected_active_variant_id {
            super::variants::ensure_active_variant(
                &transaction,
                request.recipe.photo_id,
                expected,
            )?;
        }
        let recipe = commit_recipe_in_transaction(&transaction, &request.recipe)?;
        let repository = crate::edit_repository::commit_edit_repository_in_transaction(
            &transaction,
            &request.repository,
        )?;
        transaction.commit()?;
        Ok(Some(CommitRecipeAndEditRepositoryResult {
            recipe,
            repository,
        }))
    }
}

fn source_guard_matches(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    expected: &SharedGradeSourceGuard,
) -> Result<bool, CatalogError> {
    // Select the same RAW-preferred representation and newest online location
    // as the exact photo-source owner. Only metadata is read at this boundary.
    let source = transaction
        .query_row(
            "SELECT r.id, l.display_path, r.byte_len, r.modified_at_ms
             FROM representations r
             JOIN locations l ON l.id = (
                 SELECT l2.id FROM locations l2
                 WHERE l2.representation_id = r.id AND l2.status = 'online'
                 ORDER BY l2.created_at_ms DESC, l2.id DESC LIMIT 1
             )
             WHERE r.photo_id = ?1 AND r.kind IN ('original_raw', 'original_raster')
             ORDER BY CASE r.kind WHEN 'original_raw' THEN 0 ELSE 1 END, r.created_at_ms, r.id
             LIMIT 1",
            [photo_id.as_bytes().as_slice()],
            |row| {
                Ok((
                    read_id::<RepresentationId>(row, 0)?,
                    row.get::<_, String>(1)?,
                    crate::cache_artifact::non_negative_u64(row.get(2)?, 2)?,
                    row.get::<_, Option<i64>>(3)?,
                ))
            },
        )
        .optional()?;
    Ok(source.is_some_and(|(id, path, byte_len, modified_at_ms)| {
        id == expected.representation_id
            && path == expected.source_path
            && RepresentationFingerprint {
                byte_len,
                modified_at_ms,
            } == expected.fingerprint
    }))
}

#[cfg(test)]
mod tests;
