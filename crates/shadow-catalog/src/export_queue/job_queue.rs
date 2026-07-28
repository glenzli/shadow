//! Atomic export enqueue, snapshot ownership checks, and read projections.

use rusqlite::{OptionalExtension, Transaction, params};
use shadow_domain::{EditCommitId, EntityId, PhotoId, RecipeCommitId, RepresentationId};

use crate::{
    Catalog, CatalogError,
    cache_artifact::{digest, non_negative_u64},
    row_codec::read_id,
};

use super::{
    model::{
        EnqueueExportJob, ExportItemId, ExportItemRecord, ExportJobId, ExportJobProgress,
        ExportJobRecord, MAX_EXPORT_JOB_PAGE_SIZE, NewExportItem,
    },
    presets::resolve_settings_snapshot,
    row_codec::{parse_item_state, read_export_item, read_export_job},
    validation::{json_digest, normalized_json_object, validate_output_location},
};

impl Catalog {
    /// Freezes one export job and all of its individual render snapshots.
    ///
    /// The request is validated in one transaction: the representation must
    /// belong to the photo, the Recipe snapshot digest must match the immutable
    /// commit, and an optional global edit commit must exist.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid settings/snapshots, stale ownership,
    /// a missing preset revision, or a failed atomic write.
    pub fn enqueue_export_job(
        &mut self,
        request: &EnqueueExportJob,
    ) -> Result<ExportJobRecord, CatalogError> {
        if request.items.is_empty() {
            return Err(CatalogError::InvalidExport(
                "an export job must contain at least one item".into(),
            ));
        }
        let transaction = self.connection.transaction()?;
        let (preset_revision_id, settings_json, settings_digest) =
            resolve_settings_snapshot(&transaction, &request.settings)?;
        let job_id = ExportJobId::new_v7();
        transaction.execute(
            "INSERT INTO export_jobs(
                 id, state, preset_revision_id, settings_json, settings_digest,
                 created_at_ms, updated_at_ms
             ) VALUES (?1, 'queued', ?2, ?3, ?4, ?5, ?5)",
            params![
                job_id.as_bytes().as_slice(),
                preset_revision_id
                    .as_ref()
                    .map(|id| id.as_bytes().as_slice()),
                settings_json,
                settings_digest.as_slice(),
                request.now_ms,
            ],
        )?;

        for (position, item) in request.items.iter().enumerate() {
            let position = u32::try_from(position)
                .map_err(|_| CatalogError::InvalidExport("export job has too many items".into()))?;
            insert_export_item(&transaction, job_id, position, item, request.now_ms)?;
        }
        transaction.commit()?;
        self.export_job(job_id)?
            .ok_or(CatalogError::ExportJobNotFound(job_id))
    }

    /// Reads one durable export job by id.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the row or its immutable JSON snapshot is invalid.
    pub fn export_job(&self, id: ExportJobId) -> Result<Option<ExportJobRecord>, CatalogError> {
        self.connection
            .query_row(
                "SELECT id, state, preset_revision_id, settings_json, settings_digest,
                        created_at_ms, updated_at_ms, started_at_ms, finished_at_ms,
                        last_error_code, last_error_message, last_error_retryable
                 FROM export_jobs WHERE id = ?1",
                [id.as_bytes().as_slice()],
                read_export_job,
            )
            .optional()
            .map_err(Into::into)
    }

    /// Lists the most recently updated export jobs for a task center.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid page limit or invalid stored rows.
    pub fn export_jobs(
        &self,
        requested_limit: usize,
    ) -> Result<Vec<ExportJobRecord>, CatalogError> {
        let limit = export_page_limit(requested_limit)?;
        let mut statement = self.connection.prepare(
            "SELECT id, state, preset_revision_id, settings_json, settings_digest,
                    created_at_ms, updated_at_ms, started_at_ms, finished_at_ms,
                    last_error_code, last_error_message, last_error_retryable
             FROM export_jobs ORDER BY updated_at_ms DESC, id DESC LIMIT ?1",
        )?;
        statement
            .query_map([limit], read_export_job)?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Lists all persisted items belonging to one export job in deterministic order.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the job is absent or any stored item is invalid.
    pub fn export_job_items(
        &self,
        job_id: ExportJobId,
    ) -> Result<Vec<ExportItemRecord>, CatalogError> {
        ensure_export_job_exists_connection(&self.connection, job_id)?;
        let mut statement = self.connection.prepare(
            "SELECT id, job_id, position, photo_id, representation_id, recipe_commit_id,
                    recipe_snapshot_digest, edit_commit_id, source_identity_json,
                    source_identity_digest, render_plan_json, render_plan_digest,
                    output_platform, output_native_path, output_display_path, state,
                    attempt_count, created_at_ms, updated_at_ms, started_at_ms, finished_at_ms,
                    error_code, error_message, error_retryable
             FROM export_items WHERE job_id = ?1 ORDER BY position, id",
        )?;
        statement
            .query_map([job_id.as_bytes().as_slice()], read_export_item)?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Reads one durable export item by its primary-key identity.
    ///
    /// Workers use this after claiming an item rather than enumerating its
    /// entire parent job. The primary-key lookup keeps retry and receipt paths
    /// bounded even when a job contains a very large batch.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when a stored item row is malformed or `SQLite`
    /// cannot complete the indexed lookup.
    pub fn export_item(
        &self,
        item_id: ExportItemId,
    ) -> Result<Option<ExportItemRecord>, CatalogError> {
        self.connection
            .query_row(
                "SELECT id, job_id, position, photo_id, representation_id, recipe_commit_id,
                        recipe_snapshot_digest, edit_commit_id, source_identity_json,
                        source_identity_digest, render_plan_json, render_plan_digest,
                        output_platform, output_native_path, output_display_path, state,
                        attempt_count, created_at_ms, updated_at_ms, started_at_ms, finished_at_ms,
                        error_code, error_message, error_retryable
                 FROM export_items WHERE id = ?1",
                [item_id.as_bytes().as_slice()],
                read_export_item,
            )
            .optional()
            .map_err(Into::into)
    }

    /// Counts every persisted worker state for one export job without loading
    /// its item records.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the job is absent, if a stored state is
    /// invalid, or if `SQLite` cannot aggregate the job rows.
    pub fn export_job_progress(
        &self,
        job_id: ExportJobId,
    ) -> Result<ExportJobProgress, CatalogError> {
        ensure_export_job_exists_connection(&self.connection, job_id)?;
        let mut statement = self
            .connection
            .prepare("SELECT state, COUNT(*) FROM export_items WHERE job_id = ?1 GROUP BY state")?;
        let state_counts = statement
            .query_map([job_id.as_bytes().as_slice()], |row| {
                Ok((
                    parse_item_state(&row.get::<_, String>(0)?)?,
                    non_negative_u64(row.get(1)?, 1)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        let mut progress = ExportJobProgress::default();
        for (state, count) in state_counts {
            progress.record(state, count)?;
        }
        if progress.total_item_count == 0 {
            return Err(CatalogError::InvalidExport(
                "an export job has no persisted items".into(),
            ));
        }
        Ok(progress)
    }
}

fn insert_export_item(
    transaction: &Transaction<'_>,
    job_id: ExportJobId,
    position: u32,
    item: &NewExportItem,
    now_ms: i64,
) -> Result<(), CatalogError> {
    validate_output_location(&item.output)?;
    let source_identity_json =
        normalized_json_object(&item.source_identity_json, "source identity")?;
    let render_plan_json = normalized_json_object(&item.render_plan_json, "render plan")?;
    let source_identity_digest = json_digest(&source_identity_json);
    let render_plan_digest = json_digest(&render_plan_json);
    ensure_representation_owner(transaction, item.photo_id, item.representation_id)?;
    ensure_recipe_snapshot(
        transaction,
        item.photo_id,
        item.recipe_commit_id,
        item.recipe_snapshot_digest,
    )?;
    if let Some(edit_commit_id) = item.edit_commit_id {
        ensure_edit_commit_exists(transaction, edit_commit_id)?;
    }

    let item_id = ExportItemId::new_v7();
    transaction.execute(
        "INSERT INTO export_items(
             id, job_id, position, photo_id, representation_id, recipe_commit_id,
             recipe_snapshot_digest, edit_commit_id, source_identity_json, source_identity_digest,
             render_plan_json, render_plan_digest, output_platform, output_native_path,
             output_display_path, state, created_at_ms, updated_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15,
                   'queued', ?16, ?16)",
        params![
            item_id.as_bytes().as_slice(),
            job_id.as_bytes().as_slice(),
            i64::from(position),
            item.photo_id.as_bytes().as_slice(),
            item.representation_id.as_bytes().as_slice(),
            item.recipe_commit_id.as_bytes().as_slice(),
            item.recipe_snapshot_digest.as_slice(),
            item.edit_commit_id
                .as_ref()
                .map(|id| id.as_bytes().as_slice()),
            source_identity_json,
            source_identity_digest.as_slice(),
            render_plan_json,
            render_plan_digest.as_slice(),
            item.output.platform.as_str(),
            item.output.native_path.as_slice(),
            item.output.display_path,
            now_ms,
        ],
    )?;
    Ok(())
}

pub(super) fn ensure_export_job_exists(
    transaction: &Transaction<'_>,
    job_id: ExportJobId,
) -> Result<(), CatalogError> {
    let exists = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM export_jobs WHERE id = ?1)",
        [job_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::ExportJobNotFound(job_id));
    }
    Ok(())
}

fn ensure_export_job_exists_connection(
    connection: &rusqlite::Connection,
    job_id: ExportJobId,
) -> Result<(), CatalogError> {
    let exists = connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM export_jobs WHERE id = ?1)",
        [job_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::ExportJobNotFound(job_id));
    }
    Ok(())
}

fn ensure_representation_owner(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
) -> Result<(), CatalogError> {
    let owner = transaction
        .query_row(
            "SELECT photo_id FROM representations WHERE id = ?1",
            [representation_id.as_bytes().as_slice()],
            |row| read_id::<PhotoId>(row, 0),
        )
        .optional()?;
    match owner {
        None => Err(CatalogError::RepresentationNotFound(representation_id)),
        Some(owner) if owner != photo_id => Err(CatalogError::ExportRepresentationOwnerMismatch {
            photo_id,
            representation_id,
        }),
        Some(_) => Ok(()),
    }
}

fn ensure_recipe_snapshot(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    commit_id: RecipeCommitId,
    snapshot_digest: [u8; 32],
) -> Result<(), CatalogError> {
    let stored = transaction
        .query_row(
            "SELECT snapshot_digest FROM recipe_commits WHERE photo_id = ?1 AND id = ?2",
            params![
                photo_id.as_bytes().as_slice(),
                commit_id.as_bytes().as_slice()
            ],
            |row| digest(row.get(0)?, 0),
        )
        .optional()?;
    let Some(stored) = stored else {
        return Err(CatalogError::RecipeCommitOwnerMismatch {
            photo_id,
            commit_id,
        });
    };
    if stored != snapshot_digest {
        return Err(CatalogError::ExportRecipeSnapshotMismatch { commit_id });
    }
    Ok(())
}

fn ensure_edit_commit_exists(
    transaction: &Transaction<'_>,
    edit_commit_id: EditCommitId,
) -> Result<(), CatalogError> {
    let exists = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM edit_repository_commits WHERE id = ?1)",
        [edit_commit_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::EditRepositoryCommitNotFound(edit_commit_id));
    }
    Ok(())
}

fn export_page_limit(value: usize) -> Result<i64, CatalogError> {
    if value == 0 || value > MAX_EXPORT_JOB_PAGE_SIZE {
        return Err(CatalogError::InvalidExport(format!(
            "export job page limit {value} is outside 1 through {MAX_EXPORT_JOB_PAGE_SIZE}"
        )));
    }
    i64::try_from(value)
        .map_err(|_| CatalogError::InvalidExport("export page limit overflow".into()))
}
