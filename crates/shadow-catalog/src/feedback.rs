use std::collections::BTreeSet;

use rusqlite::{OptionalExtension, Transaction, TransactionBehavior, params};
use shadow_ai::{
    FeedbackAction, FeedbackEvent, FeedbackForgetFact, LearningScope, NewFeedbackEvent,
    NewFeedbackForgetFact,
};
use shadow_domain::{EntityId, PhotoId};

use crate::{Catalog, CatalogError, cache_artifact::digest};

/// Hard upper bound for one actor query, keeping accidental full-library reads
/// off the Catalog writer thread.
pub const MAX_FEEDBACK_PAGE_SIZE: usize = 512;

/// One ascending, immutable page of human feedback facts.
#[derive(Debug, Clone, PartialEq)]
pub struct FeedbackPage {
    pub events: Vec<FeedbackEvent>,
    pub has_more: bool,
}

/// Snapshot-consistent, bounded inventory. References do not authorize training.
#[derive(Debug, serde::Serialize)]
pub struct LearningEvidencePage {
    pub report: shadow_ai::LearningReadinessReport,
    pub has_more: bool,
    pub next_after_sequence: u64,
    /// Missing histories remain human facts but are unavailable training targets.
    pub unavailable_edit_event_ids: Vec<String>,
    pub feature_artifacts_verified: bool,
    pub training_performed: bool,
}

struct StoredFeedbackRow {
    sequence: i64,
    event_id: String,
    occurred_at_ms: i64,
    scope_kind: String,
    project_id: Option<String>,
    json: String,
    digest: [u8; 32],
}

impl Catalog {
    /// Reads one evidence page, revocations and Recipe integrity in one `SQLite` snapshot.
    ///
    /// Only this page's event interval is scanned for forget facts. No image bytes,
    /// feature extraction or model execution are involved. A complete training manifest
    /// must still pin source/render identities and recheck revocations before publication.
    ///
    /// # Errors
    ///
    /// Rejects malformed evidence, unsupported bounds and corrupted Recipe history.
    pub fn learning_evidence_page(
        &self,
        scope: &LearningScope,
        after_sequence: u64,
        limit: usize,
    ) -> Result<LearningEvidencePage, CatalogError> {
        let transaction = self.connection.unchecked_transaction()?;
        let page = self.feedback_events_after(scope, after_sequence, limit)?;
        let through = page
            .events
            .last()
            .map_or(after_sequence, |event| event.sequence);
        let forgotten =
            self.forgotten_feedback_event_ids_between(scope, after_sequence, through)?;
        let report = shadow_ai::build_learning_readiness(&page.events, scope, &forgotten)
            .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;
        let mut unavailable_edit_event_ids = Vec::new();
        for example in &report.approved_edit_references {
            let before = self.recipe_commit(example.photo_id, example.baseline_recipe)?;
            let after = self.recipe_commit(example.photo_id, example.approved_recipe)?;
            if before.is_none() || after.is_none() {
                unavailable_edit_event_ids.push(example.event_id.clone());
            }
        }
        transaction.commit()?;
        Ok(LearningEvidencePage {
            report,
            has_more: page.has_more,
            next_after_sequence: through,
            unavailable_edit_event_ids,
            feature_artifacts_verified: false,
            training_performed: false,
        })
    }

    /// Validates and appends one human-feedback fact, assigning its sequence.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the fact is invalid, references an unknown
    /// photo, reuses an event id, or cannot be persisted atomically.
    pub fn append_feedback_event(
        &mut self,
        request: &NewFeedbackEvent,
    ) -> Result<FeedbackEvent, CatalogError> {
        request
            .validate()
            .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;

        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        ensure_feedback_event_absent(&transaction, &request.event_id)?;
        ensure_referenced_photos_exist(&transaction, request)?;
        ensure_presented_visual_ownership(&transaction, request)?;
        ensure_edit_example_ownership(&transaction, request)?;
        let sequence = next_feedback_sequence(&transaction)?;
        let event = request
            .clone()
            .with_sequence(sequence)
            .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;
        let event_json = canonical_json(&event)?;
        let event_digest = blake3::hash(event_json.as_bytes());
        let (scope_kind, project_id) = scope_columns(&event.scope);
        let sequence = sequence_to_sql(sequence)?;

        transaction.execute(
            "INSERT INTO ai_feedback_events(
                 sequence, event_id, occurred_at_ms, scope_kind, project_id,
                 event_json, event_digest
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            params![
                sequence,
                event.event_id,
                event.occurred_at_unix_ms,
                scope_kind,
                project_id,
                event_json,
                event_digest.as_bytes().as_slice(),
            ],
        )?;
        transaction.commit()?;
        Ok(event)
    }

    /// Returns an ascending, keyset-paginated page for exactly one scope.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid page limit, malformed scope, or
    /// persisted JSON/integrity disagreement.
    pub fn feedback_events_after(
        &self,
        scope: &LearningScope,
        after_sequence_exclusive: u64,
        limit: usize,
    ) -> Result<FeedbackPage, CatalogError> {
        validate_page_limit(limit)?;
        validate_scope(scope)?;
        let after_sequence = sequence_to_sql(after_sequence_exclusive)?;
        let query_limit =
            i64::try_from(limit + 1).map_err(|_| CatalogError::InvalidFeedbackPageLimit {
                limit,
                maximum: MAX_FEEDBACK_PAGE_SIZE,
            })?;
        let (scope_kind, project_id) = scope_columns(scope);
        let mut statement = self.connection.prepare(
            "SELECT sequence, event_id, occurred_at_ms, scope_kind, project_id,
                    event_json, event_digest
             FROM ai_feedback_events
             WHERE sequence > ?1 AND scope_kind = ?2
               AND ((?3 IS NULL AND project_id IS NULL) OR project_id = ?3)
             ORDER BY sequence ASC
             LIMIT ?4",
        )?;
        let rows = statement.query_map(
            params![after_sequence, scope_kind, project_id, query_limit],
            |row| {
                Ok(StoredFeedbackRow {
                    sequence: row.get(0)?,
                    event_id: row.get(1)?,
                    occurred_at_ms: row.get(2)?,
                    scope_kind: row.get(3)?,
                    project_id: row.get(4)?,
                    json: row.get(5)?,
                    digest: digest(row.get(6)?, 6)?,
                })
            },
        )?;

        let mut events = Vec::with_capacity(limit + 1);
        for row in rows {
            let row = row?;
            let event: FeedbackEvent =
                serde_json::from_str(&row.json).map_err(CatalogError::FeedbackJson)?;
            verify_feedback_event(&event, &row)?;
            events.push(event);
        }
        let has_more = events.len() > limit;
        events.truncate(limit);
        Ok(FeedbackPage { events, has_more })
    }

    /// Appends a non-destructive tombstone that excludes an event from future
    /// training while preserving the original evidence row.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the fact is invalid, duplicates a fact id,
    /// targets an unknown event, or cannot be persisted.
    pub fn append_feedback_forget_fact(
        &mut self,
        request: &NewFeedbackForgetFact,
    ) -> Result<FeedbackForgetFact, CatalogError> {
        request
            .validate()
            .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        ensure_forget_fact_absent(&transaction, &request.fact_id)?;
        ensure_feedback_event_exists(&transaction, &request.target_event_id)?;
        let sequence = next_forget_sequence(&transaction)?;
        let fact = request
            .clone()
            .with_sequence(sequence)
            .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;
        let fact_json = canonical_json(&fact)?;
        let fact_digest = blake3::hash(fact_json.as_bytes());
        let sequence = sequence_to_sql(sequence)?;
        transaction.execute(
            "INSERT INTO ai_feedback_forget_facts(
                 sequence, fact_id, target_event_id, occurred_at_ms, fact_json, fact_digest
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![
                sequence,
                fact.fact_id,
                fact.target_event_id,
                fact.occurred_at_unix_ms,
                fact_json,
                fact_digest.as_bytes().as_slice(),
            ],
        )?;
        transaction.commit()?;
        Ok(fact)
    }

    /// Returns target event ids with one or more durable forget facts in a scope.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the scope is malformed or the query fails.
    pub fn forgotten_feedback_event_ids(
        &self,
        scope: &LearningScope,
    ) -> Result<BTreeSet<String>, CatalogError> {
        self.forgotten_feedback_event_ids_between(scope, 0, i64::MAX.unsigned_abs())
    }

    fn forgotten_feedback_event_ids_between(
        &self,
        scope: &LearningScope,
        after: u64,
        through: u64,
    ) -> Result<BTreeSet<String>, CatalogError> {
        validate_scope(scope)?;
        let (scope_kind, project_id) = scope_columns(scope);
        let mut statement = self.connection.prepare(
            "SELECT f.sequence, f.fact_id, f.target_event_id, f.occurred_at_ms,
                    f.fact_json, f.fact_digest
             FROM ai_feedback_forget_facts f
             JOIN ai_feedback_events e ON e.event_id = f.target_event_id
             WHERE e.scope_kind = ?1
               AND ((?2 IS NULL AND e.project_id IS NULL) OR e.project_id = ?2)
             AND e.sequence > ?3 AND e.sequence <= ?4
             ORDER BY f.sequence ASC",
        )?;
        let rows = statement.query_map(
            params![
                scope_kind,
                project_id,
                sequence_to_sql(after)?,
                sequence_to_sql(through)?
            ],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, i64>(3)?,
                    row.get::<_, String>(4)?,
                    digest(row.get(5)?, 5)?,
                ))
            },
        )?;
        let mut event_ids = BTreeSet::new();
        for row in rows {
            let (sequence, fact_id, target_event_id, occurred_at_ms, json, stored_digest) = row?;
            let fact: FeedbackForgetFact =
                serde_json::from_str(&json).map_err(CatalogError::FeedbackJson)?;
            verify_feedback_forget_fact(
                &fact,
                sequence,
                &fact_id,
                &target_event_id,
                occurred_at_ms,
                &json,
                stored_digest,
            )?;
            event_ids.insert(target_event_id);
        }
        Ok(event_ids)
    }
}

fn validate_page_limit(limit: usize) -> Result<(), CatalogError> {
    if !(1..=MAX_FEEDBACK_PAGE_SIZE).contains(&limit) {
        return Err(CatalogError::InvalidFeedbackPageLimit {
            limit,
            maximum: MAX_FEEDBACK_PAGE_SIZE,
        });
    }
    Ok(())
}

fn validate_scope(scope: &LearningScope) -> Result<(), CatalogError> {
    if let LearningScope::Project { project_id } = scope
        && (project_id.trim().is_empty() || project_id.len() > 256)
    {
        return Err(CatalogError::InvalidFeedback(
            "project id must contain 1 through 256 bytes".into(),
        ));
    }
    Ok(())
}

fn scope_columns(scope: &LearningScope) -> (&'static str, Option<&str>) {
    match scope {
        LearningScope::Global => ("global", None),
        LearningScope::Project { project_id } => ("project", Some(project_id.as_str())),
    }
}

fn canonical_json<T: serde::Serialize>(value: &T) -> Result<String, CatalogError> {
    serde_json::to_string(value).map_err(CatalogError::FeedbackJson)
}

fn verify_feedback_event(
    event: &FeedbackEvent,
    stored: &StoredFeedbackRow,
) -> Result<(), CatalogError> {
    event
        .validate()
        .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;
    let sequence = sequence_from_sql(stored.sequence)?;
    if event.sequence != sequence
        || event.event_id != stored.event_id
        || event.occurred_at_unix_ms != stored.occurred_at_ms
    {
        return Err(CatalogError::InvalidPersistedFeedback(
            "event JSON identity disagrees with indexed columns",
        ));
    }
    let (scope_kind, project_id) = scope_columns(&event.scope);
    if scope_kind != stored.scope_kind || project_id != stored.project_id.as_deref() {
        return Err(CatalogError::InvalidPersistedFeedback(
            "event JSON scope disagrees with indexed columns",
        ));
    }
    let canonical = canonical_json(event)?;
    if canonical != stored.json {
        return Err(CatalogError::InvalidPersistedFeedback(
            "event JSON is not canonical",
        ));
    }
    if blake3::hash(stored.json.as_bytes()).as_bytes() != &stored.digest {
        return Err(CatalogError::InvalidPersistedFeedback(
            "event JSON digest does not match",
        ));
    }
    Ok(())
}

fn verify_feedback_forget_fact(
    fact: &FeedbackForgetFact,
    stored_sequence: i64,
    stored_fact_id: &str,
    stored_target_event_id: &str,
    stored_occurred_at_ms: i64,
    stored_json: &str,
    stored_digest: [u8; 32],
) -> Result<(), CatalogError> {
    fact.validate()
        .map_err(|error| CatalogError::InvalidFeedback(error.to_string()))?;
    let sequence = sequence_from_sql(stored_sequence)?;
    if fact.sequence != sequence
        || fact.fact_id != stored_fact_id
        || fact.target_event_id != stored_target_event_id
        || fact.occurred_at_unix_ms != stored_occurred_at_ms
    {
        return Err(CatalogError::InvalidPersistedFeedback(
            "forget JSON identity disagrees with indexed columns",
        ));
    }
    let canonical = canonical_json(fact)?;
    if canonical != stored_json {
        return Err(CatalogError::InvalidPersistedFeedback(
            "forget JSON is not canonical",
        ));
    }
    if blake3::hash(stored_json.as_bytes()).as_bytes() != &stored_digest {
        return Err(CatalogError::InvalidPersistedFeedback(
            "forget JSON digest does not match",
        ));
    }
    Ok(())
}

fn next_feedback_sequence(transaction: &Transaction<'_>) -> Result<u64, CatalogError> {
    next_sequence(
        transaction,
        "SELECT COALESCE(MAX(sequence), 0) FROM ai_feedback_events",
    )
}

fn next_forget_sequence(transaction: &Transaction<'_>) -> Result<u64, CatalogError> {
    next_sequence(
        transaction,
        "SELECT COALESCE(MAX(sequence), 0) FROM ai_feedback_forget_facts",
    )
}

fn next_sequence(transaction: &Transaction<'_>, sql: &str) -> Result<u64, CatalogError> {
    let current: i64 = transaction.query_row(sql, [], |row| row.get(0))?;
    let next = current
        .checked_add(1)
        .ok_or(CatalogError::FeedbackSequenceExhausted)?;
    sequence_from_sql(next)
}

fn sequence_to_sql(sequence: u64) -> Result<i64, CatalogError> {
    i64::try_from(sequence).map_err(|_| CatalogError::FeedbackSequenceExhausted)
}

fn sequence_from_sql(sequence: i64) -> Result<u64, CatalogError> {
    u64::try_from(sequence).map_err(|_| CatalogError::InvalidPersistedFeedback("invalid sequence"))
}

fn ensure_feedback_event_absent(
    transaction: &Transaction<'_>,
    event_id: &str,
) -> Result<(), CatalogError> {
    let exists = transaction
        .query_row(
            "SELECT 1 FROM ai_feedback_events WHERE event_id = ?1",
            [event_id],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        Err(CatalogError::FeedbackEventAlreadyExists(
            event_id.to_owned(),
        ))
    } else {
        Ok(())
    }
}

fn ensure_feedback_event_exists(
    transaction: &Transaction<'_>,
    event_id: &str,
) -> Result<(), CatalogError> {
    let exists = transaction
        .query_row(
            "SELECT 1 FROM ai_feedback_events WHERE event_id = ?1",
            [event_id],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        Ok(())
    } else {
        Err(CatalogError::FeedbackEventNotFound(event_id.to_owned()))
    }
}

fn ensure_forget_fact_absent(
    transaction: &Transaction<'_>,
    fact_id: &str,
) -> Result<(), CatalogError> {
    let exists = transaction
        .query_row(
            "SELECT 1 FROM ai_feedback_forget_facts WHERE fact_id = ?1",
            [fact_id],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        Err(CatalogError::FeedbackForgetFactAlreadyExists(
            fact_id.to_owned(),
        ))
    } else {
        Ok(())
    }
}

fn ensure_referenced_photos_exist(
    transaction: &Transaction<'_>,
    request: &NewFeedbackEvent,
) -> Result<(), CatalogError> {
    let mut photo_ids = request
        .presentation
        .candidates
        .iter()
        .map(|candidate| candidate.photo_id)
        .collect::<BTreeSet<_>>();
    match &request.action {
        FeedbackAction::PairwiseComparison { left, right, .. } => {
            photo_ids.insert(*left);
            photo_ids.insert(*right);
        }
        FeedbackAction::EditExampleConfirmed { photo_id, .. }
        | FeedbackAction::FlagChanged { photo_id, .. }
        | FeedbackAction::RatingChanged { photo_id, .. }
        | FeedbackAction::Exported { photo_id }
        | FeedbackAction::ReturnedForRework { photo_id } => {
            photo_ids.insert(*photo_id);
        }
        FeedbackAction::ParametersCopied {
            source_photo,
            target_photos,
        } => {
            photo_ids.insert(*source_photo);
            photo_ids.extend(target_photos.iter().copied());
        }
        FeedbackAction::SuggestionReviewed { .. }
        | FeedbackAction::DevelopProposalEdited { .. } => {}
    }
    for photo_id in photo_ids {
        ensure_photo_exists(transaction, photo_id)?;
    }
    Ok(())
}

fn ensure_photo_exists(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let exists = transaction
        .query_row(
            "SELECT 1 FROM photos WHERE id = ?1",
            [photo_id.as_bytes().as_slice()],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        Ok(())
    } else {
        Err(CatalogError::PhotoNotFound(photo_id))
    }
}

fn ensure_presented_visual_ownership(
    transaction: &Transaction<'_>,
    request: &NewFeedbackEvent,
) -> Result<(), CatalogError> {
    for candidate in &request.presentation.candidates {
        let Some(visual) = &candidate.visual else {
            continue;
        };
        let representation_id = visual.artifact.representation_id;
        let belongs_to_candidate = transaction
            .query_row(
                "SELECT 1 FROM representations WHERE id = ?1 AND photo_id = ?2",
                params![
                    representation_id.as_bytes().as_slice(),
                    candidate.photo_id.as_bytes().as_slice()
                ],
                |_| Ok(()),
            )
            .optional()?
            .is_some();
        if !belongs_to_candidate {
            return Err(CatalogError::FeedbackVisualRepresentationOwnerMismatch {
                photo_id: candidate.photo_id,
                representation_id,
            });
        }
    }
    Ok(())
}

/// Bind approval to existing immutable commits belonging to exactly this logical photo.
fn ensure_edit_example_ownership(
    transaction: &Transaction<'_>,
    request: &NewFeedbackEvent,
) -> Result<(), CatalogError> {
    if let FeedbackAction::EditExampleConfirmed {
        photo_id,
        baseline_recipe,
        approved_recipe,
        ..
    } = request.action
    {
        for commit_id in [baseline_recipe, approved_recipe] {
            let exists = transaction
                .query_row(
                    "SELECT 1 FROM recipe_commits WHERE photo_id = ?1 AND id = ?2",
                    params![
                        photo_id.as_bytes().as_slice(),
                        commit_id.as_bytes().as_slice()
                    ],
                    |_| Ok(()),
                )
                .optional()?
                .is_some();
            if !exists {
                return Err(CatalogError::InvalidFeedback(
                    "edit example commits must exist and belong to the declared photo".into(),
                ));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;
