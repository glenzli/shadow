//! Resumable worker transitions, receipts, cancellation, and startup recovery.

use rusqlite::{OptionalExtension, Transaction, params};
use shadow_domain::EntityId;

use crate::{Catalog, CatalogError, cache_artifact::non_negative_u64, row_codec::read_id};

use super::{
    job_queue::ensure_export_job_exists,
    model::{
        AdvanceExportItem, ExportItemId, ExportItemRecord, ExportItemState, ExportJobId,
        ExportJobRecord, ExportJobState, ExportOutputReceiptId, ExportOutputReceiptRecord,
        ExportQueueRecovery, NewExportOutputReceipt,
    },
    row_codec::{parse_item_state, read_export_item, read_export_receipt, read_failure},
    validation::{normalized_json_object, validate_failure, validate_receipt},
};

impl Catalog {
    /// Claims the oldest queued item atomically, moving it to `Preparing`.
    ///
    /// The Catalog actor serializes callers today; keeping this operation
    /// transactional also makes the invariant explicit for future workers.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when queue persistence or invariant checks fail.
    pub fn claim_next_export_item(
        &mut self,
        now_ms: i64,
    ) -> Result<Option<ExportItemRecord>, CatalogError> {
        let transaction = self.connection.transaction()?;
        let id = transaction
            .query_row(
                "SELECT id FROM export_items WHERE state = 'queued'
                 ORDER BY created_at_ms, job_id, position, id LIMIT 1",
                [],
                |row| read_id::<ExportItemId>(row, 0),
            )
            .optional()?;
        let Some(id) = id else {
            transaction.commit()?;
            return Ok(None);
        };
        let item = transition_export_item_in_transaction(
            &transaction,
            &AdvanceExportItem {
                item_id: id,
                expected_state: ExportItemState::Queued,
                next_state: ExportItemState::Preparing,
                failure: None,
                receipt: None,
                now_ms,
            },
        )?;
        transaction.commit()?;
        Ok(Some(item))
    }

    /// Advances one worker item using an explicit expected state.
    ///
    /// A failed compare-and-swap reports the actual state rather than letting a
    /// late renderer overwrite output from a retry or cancellation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for a stale or invalid transition, invalid
    /// result payload, or a failed transaction.
    pub fn advance_export_item(
        &mut self,
        request: &AdvanceExportItem,
    ) -> Result<ExportItemRecord, CatalogError> {
        let transaction = self.connection.transaction()?;
        let record = transition_export_item_in_transaction(&transaction, request)?;
        transaction.commit()?;
        Ok(record)
    }

    /// Returns the immutable output receipt recorded for one completed item.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the persisted receipt is malformed.
    pub fn export_output_receipt(
        &self,
        item_id: ExportItemId,
    ) -> Result<Option<ExportOutputReceiptRecord>, CatalogError> {
        self.connection
            .query_row(
                "SELECT id, item_id, output_platform, output_native_path, output_display_path,
                        output_format, byte_len, content_digest, receipt_json, created_at_ms
                 FROM export_output_receipts WHERE item_id = ?1",
                [item_id.as_bytes().as_slice()],
                read_export_receipt,
            )
            .optional()
            .map_err(Into::into)
    }

    /// Cancels every nonterminal item in a job and leaves completed outputs intact.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the job is absent or the transaction fails.
    pub fn cancel_export_job(
        &mut self,
        job_id: ExportJobId,
        now_ms: i64,
    ) -> Result<ExportJobRecord, CatalogError> {
        let transaction = self.connection.transaction()?;
        ensure_export_job_exists(&transaction, job_id)?;
        transaction.execute(
            "UPDATE export_items
             SET state = 'cancelled', updated_at_ms = ?2, finished_at_ms = ?2,
                 error_code = NULL, error_message = NULL, error_retryable = NULL
             WHERE job_id = ?1 AND state NOT IN ('cancelled', 'failed', 'completed')",
            params![job_id.as_bytes().as_slice(), now_ms],
        )?;
        refresh_export_job_state(&transaction, job_id, now_ms)?;
        transaction.commit()?;
        self.export_job(job_id)?
            .ok_or(CatalogError::ExportJobNotFound(job_id))
    }

    /// Atomically makes all safely retryable export work available after an
    /// unclean process exit.
    ///
    /// Active worker states cannot have published a durable output without an
    /// immutable receipt. They may therefore return directly to `Queued`.
    /// Existing `Interrupted` rows from an older recovery pass are included as
    /// well. Frozen Recipe, source, render-plan, and output snapshots are never
    /// updated; only transient worker state and timings are cleared.
    ///
    /// This deliberately operates on the full durable queue. It does not use
    /// the task-center page limit, so a startup does not strand work after the
    /// first 256 jobs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when recovery cannot be committed atomically.
    pub fn recover_and_requeue_interrupted_export_items(
        &mut self,
        now_ms: i64,
    ) -> Result<ExportQueueRecovery, CatalogError> {
        let transaction = self.connection.transaction()?;
        let mut statement = transaction.prepare(
            "SELECT DISTINCT job_id FROM export_items
             WHERE state IN ('preparing', 'rendering', 'encoding', 'writing_temp', 'interrupted')",
        )?;
        let job_ids = statement
            .query_map([], |row| read_id::<ExportJobId>(row, 0))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        drop(statement);
        let interrupted_item_count = non_negative_u64(
            transaction.query_row(
                "SELECT COUNT(*) FROM export_items
                 WHERE state IN ('preparing', 'rendering', 'encoding', 'writing_temp')",
                [],
                |row| row.get(0),
            )?,
            0,
        )?;
        let requeued_item_count = u64::try_from(transaction.execute(
            "UPDATE export_items
             SET state = 'queued', updated_at_ms = ?1,
                 started_at_ms = NULL, finished_at_ms = NULL,
                 error_code = NULL, error_message = NULL, error_retryable = NULL
             WHERE state IN ('preparing', 'rendering', 'encoding', 'writing_temp', 'interrupted')",
            [now_ms],
        )?)
        .map_err(|_| {
            CatalogError::InvalidExport("recovered export item count exceeds u64".into())
        })?;
        for job_id in job_ids {
            refresh_export_job_state(&transaction, job_id, now_ms)?;
        }
        let queued_item_count = non_negative_u64(
            transaction.query_row(
                "SELECT COUNT(*) FROM export_items WHERE state = 'queued'",
                [],
                |row| row.get(0),
            )?,
            0,
        )?;
        transaction.commit()?;
        Ok(ExportQueueRecovery {
            interrupted_item_count,
            requeued_item_count,
            queued_item_count,
        })
    }

    /// Compatibility facade for callers that only need the number of items
    /// made available by startup recovery.
    ///
    /// New callers should use
    /// [`Catalog::recover_and_requeue_interrupted_export_items`] to receive
    /// exact recovery and global-queue counts without a follow-up scan.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when queue recovery cannot be persisted.
    pub fn recover_interrupted_export_jobs(&mut self, now_ms: i64) -> Result<usize, CatalogError> {
        let recovery = self.recover_and_requeue_interrupted_export_items(now_ms)?;
        usize::try_from(recovery.requeued_item_count).map_err(|_| {
            CatalogError::InvalidExport("recovered export item count exceeds usize".into())
        })
    }
}

fn transition_export_item_in_transaction(
    transaction: &Transaction<'_>,
    request: &AdvanceExportItem,
) -> Result<ExportItemRecord, CatalogError> {
    let current = export_item_in_transaction(transaction, request.item_id)?
        .ok_or(CatalogError::ExportItemNotFound(request.item_id))?;
    if current.state != request.expected_state {
        return Err(CatalogError::ExportItemStateMismatch {
            item_id: request.item_id,
            expected: request.expected_state,
            actual: current.state,
        });
    }
    if !current.state.allows_transition_to(request.next_state) {
        return Err(CatalogError::InvalidExportTransition {
            item_id: request.item_id,
            from: current.state,
            to: request.next_state,
        });
    }
    validate_transition_payload(request)?;
    if let Some(receipt) = request.receipt.as_ref() {
        validate_receipt(receipt, &current.output)?;
    }

    let starting = request.next_state == ExportItemState::Preparing;
    let finishing =
        request.next_state.is_terminal() || request.next_state == ExportItemState::Interrupted;
    let failure = request.failure.as_ref();
    transaction.execute(
        "UPDATE export_items
         SET state = ?2,
             attempt_count = attempt_count + CASE WHEN ?3 THEN 1 ELSE 0 END,
             updated_at_ms = ?4,
             started_at_ms = CASE
                 WHEN ?3 THEN COALESCE(started_at_ms, ?4)
                 WHEN ?2 = 'queued' THEN NULL
                 ELSE started_at_ms
             END,
             finished_at_ms = CASE WHEN ?5 THEN ?4 WHEN ?2 = 'queued' THEN NULL ELSE finished_at_ms END,
             error_code = ?6,
             error_message = ?7,
             error_retryable = ?8
         WHERE id = ?1",
        params![
            request.item_id.as_bytes().as_slice(),
            request.next_state.as_str(),
            starting,
            request.now_ms,
            finishing,
            failure.map(|value| value.code.as_str()),
            failure.map(|value| value.message.as_str()),
            failure.map(|value| i64::from(value.retryable)),
        ],
    )?;
    if let Some(receipt) = request.receipt.as_ref() {
        insert_receipt(transaction, request.item_id, receipt, request.now_ms)?;
    }
    refresh_export_job_state(transaction, current.job_id, request.now_ms)?;
    export_item_in_transaction(transaction, request.item_id)?
        .ok_or(CatalogError::ExportItemNotFound(request.item_id))
}

fn validate_transition_payload(request: &AdvanceExportItem) -> Result<(), CatalogError> {
    match request.next_state {
        ExportItemState::Failed | ExportItemState::Interrupted => {
            let failure = request.failure.as_ref().ok_or_else(|| {
                CatalogError::InvalidExport(
                    "a failed or interrupted export item requires failure details".into(),
                )
            })?;
            validate_failure(failure)?;
            if request.receipt.is_some() {
                return Err(CatalogError::InvalidExport(
                    "a failed or interrupted export item cannot record an output receipt".into(),
                ));
            }
        }
        ExportItemState::Completed => {
            if request.failure.is_some() {
                return Err(CatalogError::InvalidExport(
                    "a completed export item cannot contain failure details".into(),
                ));
            }
            if request.receipt.is_none() {
                return Err(CatalogError::InvalidExport(
                    "a completed export item requires an output receipt".into(),
                ));
            }
        }
        _ => {
            if request.failure.is_some() || request.receipt.is_some() {
                return Err(CatalogError::InvalidExport(
                    "only failed or completed export transitions may carry result data".into(),
                ));
            }
        }
    }
    Ok(())
}

fn insert_receipt(
    transaction: &Transaction<'_>,
    item_id: ExportItemId,
    receipt: &NewExportOutputReceipt,
    now_ms: i64,
) -> Result<(), CatalogError> {
    let id = ExportOutputReceiptId::new_v7();
    let byte_len = i64::try_from(receipt.byte_len).map_err(|_| {
        CatalogError::InvalidExport("export output byte length exceeds SQLite integer range".into())
    })?;
    let receipt_json = normalized_json_object(&receipt.receipt_json, "output receipt")?;
    transaction.execute(
        "INSERT INTO export_output_receipts(
             id, item_id, output_platform, output_native_path, output_display_path,
             output_format, byte_len, content_digest, receipt_json, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
        params![
            id.as_bytes().as_slice(),
            item_id.as_bytes().as_slice(),
            receipt.output.platform.as_str(),
            receipt.output.native_path.as_slice(),
            receipt.output.display_path,
            receipt.output_format,
            byte_len,
            receipt.content_digest.as_ref().map(<[u8; 32]>::as_slice),
            receipt_json,
            now_ms,
        ],
    )?;
    Ok(())
}

fn refresh_export_job_state(
    transaction: &Transaction<'_>,
    job_id: ExportJobId,
    now_ms: i64,
) -> Result<(), CatalogError> {
    let mut statement = transaction
        .prepare("SELECT state, COUNT(*) FROM export_items WHERE job_id = ?1 GROUP BY state")?;
    let state_counts = statement
        .query_map([job_id.as_bytes().as_slice()], |row| {
            Ok((
                parse_item_state(&row.get::<_, String>(0)?)?,
                row.get::<_, i64>(1)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    drop(statement);
    let state = derive_job_state(&state_counts)?;
    let latest_failure = transaction
        .query_row(
            "SELECT error_code, error_message, error_retryable
             FROM export_items WHERE job_id = ?1 AND state IN ('failed', 'interrupted')
             ORDER BY updated_at_ms DESC, id DESC LIMIT 1",
            [job_id.as_bytes().as_slice()],
            |row| read_failure(row, 0),
        )
        .optional()?
        .flatten();
    transaction.execute(
        "UPDATE export_jobs
         SET state = ?2, updated_at_ms = ?3,
             started_at_ms = CASE
                 WHEN ?2 = 'running' THEN COALESCE(started_at_ms, ?3)
                 WHEN ?2 = 'queued' THEN NULL
                 ELSE started_at_ms
             END,
             finished_at_ms = CASE WHEN ?4 THEN COALESCE(finished_at_ms, ?3) ELSE NULL END,
             last_error_code = ?5, last_error_message = ?6, last_error_retryable = ?7
         WHERE id = ?1",
        params![
            job_id.as_bytes().as_slice(),
            state.as_str(),
            now_ms,
            state.is_terminal(),
            latest_failure.as_ref().map(|value| value.code.as_str()),
            latest_failure.as_ref().map(|value| value.message.as_str()),
            latest_failure
                .as_ref()
                .map(|value| i64::from(u8::from(value.retryable))),
        ],
    )?;
    Ok(())
}

fn derive_job_state(
    state_counts: &[(ExportItemState, i64)],
) -> Result<ExportJobState, CatalogError> {
    let count = |expected| {
        state_counts
            .iter()
            .find_map(|(state, count)| (*state == expected).then_some(*count))
            .unwrap_or(0)
    };
    let total = state_counts.iter().try_fold(0_i64, |sum, (_, value)| {
        sum.checked_add(*value).ok_or_else(|| {
            CatalogError::InvalidExport("export item count overflowed SQLite integer range".into())
        })
    })?;
    if total <= 0 {
        return Err(CatalogError::InvalidExport(
            "an export job has no persisted items".into(),
        ));
    }
    if count(ExportItemState::PausedConflict) > 0 {
        return Ok(ExportJobState::PausedConflict);
    }
    if count(ExportItemState::Preparing) > 0
        || count(ExportItemState::Rendering) > 0
        || count(ExportItemState::Encoding) > 0
        || count(ExportItemState::WritingTemp) > 0
    {
        return Ok(ExportJobState::Running);
    }
    if count(ExportItemState::Queued) > 0 {
        return if count(ExportItemState::Failed) > 0
            || count(ExportItemState::Interrupted) > 0
            || count(ExportItemState::Completed) > 0
        {
            Ok(ExportJobState::Running)
        } else {
            Ok(ExportJobState::Queued)
        };
    }
    if count(ExportItemState::Failed) > 0 {
        return Ok(ExportJobState::Failed);
    }
    if count(ExportItemState::Interrupted) > 0 {
        return Ok(ExportJobState::Interrupted);
    }
    if count(ExportItemState::Completed) == total {
        return Ok(ExportJobState::Completed);
    }
    if count(ExportItemState::Cancelled) == total {
        return Ok(ExportJobState::Cancelled);
    }
    // A mixed completed/cancelled terminal job has intentionally stopped and
    // is reported as cancelled rather than pretending all requested outputs
    // were successful.
    Ok(ExportJobState::Cancelled)
}

fn export_item_in_transaction(
    transaction: &Transaction<'_>,
    item_id: ExportItemId,
) -> Result<Option<ExportItemRecord>, CatalogError> {
    transaction
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
