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
        validate_scope(scope)?;
        let (scope_kind, project_id) = scope_columns(scope);
        let mut statement = self.connection.prepare(
            "SELECT f.sequence, f.fact_id, f.target_event_id, f.occurred_at_ms,
                    f.fact_json, f.fact_digest
             FROM ai_feedback_forget_facts f
             JOIN ai_feedback_events e ON e.event_id = f.target_event_id
             WHERE e.scope_kind = ?1
               AND ((?2 IS NULL AND e.project_id IS NULL) OR e.project_id = ?2)
             ORDER BY f.sequence ASC",
        )?;
        let rows = statement.query_map(params![scope_kind, project_id], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
                row.get::<_, i64>(3)?,
                row.get::<_, String>(4)?,
                digest(row.get(5)?, 5)?,
            ))
        })?;
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
        FeedbackAction::FlagChanged { photo_id, .. }
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

#[cfg(test)]
mod tests {
    use std::collections::{BTreeMap, BTreeSet};

    use shadow_ai::{
        FeatureSnapshotRef, IncrementalTrainingPolicy, PairwiseOutcome, PresentationContext,
        PresentedCandidate, PresentedFitMode, PresentedVisualArtifact, PresentedVisualFrame,
        PresentedVisualProvenance, PresentedVisualRole, UnitInterval,
        build_incremental_preference_batch,
    };
    use shadow_domain::{
        AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RecipeCommitId,
        RepresentationId, RepresentationKind,
    };

    use super::*;
    use crate::{
        CachedArtifact, CachedArtifactRole, InvalidateCachedArtifactStatus, RecordCachedArtifact,
        RecordCachedArtifactStatus, RegisterAsset, RegisteredAsset, RegistrationStatus,
        RepresentationFingerprint,
    };

    fn register_source(catalog: &mut Catalog, index: u32) -> RegisteredAsset {
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    format!("/photos/feedback-{index}.dng").into_bytes(),
                    format!("/photos/feedback-{index}.dng"),
                ),
                byte_len: 42,
                modified_at_ms: Some(i64::from(index)),
                now_ms: 1_700_000_000_000 + i64::from(index),
            })
            .expect("register feedback photo");
        assert_eq!(registered.status, RegistrationStatus::Inserted);
        registered
    }

    fn register_photo(catalog: &mut Catalog, index: u32) -> PhotoId {
        register_source(catalog, index).photo_id
    }

    fn presentation(candidates: Vec<PresentedCandidate>) -> PresentationContext {
        PresentationContext {
            session_id: "review-session".into(),
            group_id: None,
            candidates,
            active_model: None,
        }
    }

    fn candidate(photo_id: PhotoId, position: u32, hash: &str) -> PresentedCandidate {
        PresentedCandidate {
            photo_id,
            position,
            visible_fraction: UnitInterval::ONE,
            inspected_at_one_to_one: false,
            feature: Some(FeatureSnapshotRef {
                extractor_id: "frozen-features".into(),
                extractor_revision: "r1".into(),
                preprocessing_version: "display-proxy-v1".into(),
                artifact_hash: hash.into(),
                dimension: 3,
            }),
            visual: None,
        }
    }

    fn presented_visual(representation_id: RepresentationId) -> PresentedVisualProvenance {
        PresentedVisualProvenance {
            artifact: PresentedVisualArtifact {
                representation_id,
                source_byte_len: 42,
                source_modified_at_ms: Some(1),
                role: PresentedVisualRole::EmbeddedPreview,
                variant_key: "embedded-0".into(),
                generator_id: "test-preview-extractor".into(),
                generator_version: "1".into(),
                provider_preview_id: Some(0),
                blob_algorithm: "blake3".into(),
                blob_digest_hex: "07".repeat(32),
                blob_byte_len: 2_048,
                codec: "jpeg".into(),
                byte_order: "not_applicable".into(),
                width: 1_920,
                height: 1_280,
                bits_per_channel: 8,
                channels: 3,
                created_at_ms: 1_700_000_000_100,
            },
            frame: PresentedVisualFrame {
                surface_id: "review-compare-left".into(),
                surface_revision: 1,
                fit_mode: PresentedFitMode::PreserveAspectFit,
                decoder_id: "qt-image-jpeg".into(),
                decoder_version: "6.8.3".into(),
                auto_transform: true,
                requested_width: 960,
                requested_height: 640,
                decoded_width: 1_920,
                decoded_height: 1_280,
                pixel_format: "rgba8888-premultiplied".into(),
                pixel_hash_algorithm: "blake3".into(),
                pixel_hash_hex: "09".repeat(32),
            },
        }
    }

    fn exported_event(event_id: &str, scope: LearningScope, photo_id: PhotoId) -> NewFeedbackEvent {
        NewFeedbackEvent {
            event_id: event_id.into(),
            occurred_at_unix_ms: 1_700_000_001_000,
            scope,
            presentation: presentation(vec![]),
            action: FeedbackAction::Exported { photo_id },
        }
    }

    fn pairwise_event(
        event_id: &str,
        scope: LearningScope,
        left: PhotoId,
        right: PhotoId,
    ) -> NewFeedbackEvent {
        NewFeedbackEvent {
            event_id: event_id.into(),
            occurred_at_unix_ms: 1_700_000_001_000,
            scope,
            presentation: presentation(vec![
                candidate(left, 0, "left-feature"),
                candidate(right, 1, "right-feature"),
            ]),
            action: FeedbackAction::PairwiseComparison {
                left,
                right,
                outcome: PairwiseOutcome::LeftPreferred,
            },
        }
    }

    fn catalog_with_exported_event(event_id: &str) -> Catalog {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo = register_photo(&mut catalog, 1);
        catalog
            .append_feedback_event(&exported_event(event_id, LearningScope::Global, photo))
            .expect("append exported event");
        catalog
    }

    #[test]
    fn migration_six_creates_immutable_feedback_storage() {
        let catalog = Catalog::open_in_memory().expect("open catalog");
        assert_eq!(catalog.schema_version().expect("schema version"), 9);
        let tables: i64 = catalog
            .connection
            .query_row(
                "SELECT COUNT(*) FROM sqlite_schema
                 WHERE type = 'table' AND name IN (
                     'ai_feedback_events', 'ai_feedback_forget_facts'
                 )",
                [],
                |row| row.get(0),
            )
            .expect("query feedback tables");
        let triggers: i64 = catalog
            .connection
            .query_row(
                "SELECT COUNT(*) FROM sqlite_schema
                 WHERE type = 'trigger' AND name LIKE 'ai_feedback_%_no_%'",
                [],
                |row| row.get(0),
            )
            .expect("query feedback triggers");
        assert_eq!(tables, 2);
        assert_eq!(triggers, 4);
    }

    #[test]
    fn catalog_assigns_global_sequence_and_pages_each_scope_after_reopen() {
        let root = std::env::temp_dir().join(format!("shadow-feedback-{}", PhotoId::new_v7()));
        std::fs::create_dir_all(&root).expect("create feedback fixture");
        let path = root.join("catalog.sqlite");
        let (first_photo, second_photo);
        {
            let mut catalog = Catalog::open(&path).expect("open catalog");
            first_photo = register_photo(&mut catalog, 1);
            second_photo = register_photo(&mut catalog, 2);
            let first = catalog
                .append_feedback_event(&exported_event(
                    "event-global-1",
                    LearningScope::Global,
                    first_photo,
                ))
                .expect("append first event");
            let project = catalog
                .append_feedback_event(&exported_event(
                    "event-project-1",
                    LearningScope::Project {
                        project_id: "wedding".into(),
                    },
                    second_photo,
                ))
                .expect("append project event");
            let second = catalog
                .append_feedback_event(&exported_event(
                    "event-global-2",
                    LearningScope::Global,
                    second_photo,
                ))
                .expect("append second event");
            assert_eq!(
                (first.sequence, project.sequence, second.sequence),
                (1, 2, 3)
            );
        }

        {
            let catalog = Catalog::open(&path).expect("reopen catalog");
            let first_page = catalog
                .feedback_events_after(&LearningScope::Global, 0, 1)
                .expect("read first global page");
            assert!(first_page.has_more);
            assert_eq!(first_page.events[0].event_id, "event-global-1");
            let second_page = catalog
                .feedback_events_after(&LearningScope::Global, first_page.events[0].sequence, 1)
                .expect("read second global page");
            assert!(!second_page.has_more);
            assert_eq!(second_page.events[0].event_id, "event-global-2");

            let project_page = catalog
                .feedback_events_after(
                    &LearningScope::Project {
                        project_id: "wedding".into(),
                    },
                    0,
                    10,
                )
                .expect("read project page");
            assert_eq!(project_page.events.len(), 1);
            assert_eq!(project_page.events[0].event_id, "event-project-1");
        }
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn duplicate_event_id_is_rejected_without_overwriting_source_fact() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo = register_photo(&mut catalog, 1);
        let original = exported_event("event-1", LearningScope::Global, photo);
        catalog
            .append_feedback_event(&original)
            .expect("append original");
        let mut replacement = original;
        replacement.occurred_at_unix_ms += 1;
        let error = catalog
            .append_feedback_event(&replacement)
            .expect_err("duplicate event must fail");
        assert!(matches!(error, CatalogError::FeedbackEventAlreadyExists(id) if id == "event-1"));
        let stored = catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read original");
        assert_eq!(stored.events.len(), 1);
        assert_eq!(stored.events[0].occurred_at_unix_ms, 1_700_000_001_000);
    }

    #[test]
    fn malformed_evidence_and_unknown_photos_fail_before_persistence() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo = register_photo(&mut catalog, 1);

        let mut duplicate_candidate = pairwise_event(
            "duplicate-candidate",
            LearningScope::Global,
            photo,
            PhotoId::new_v7(),
        );
        duplicate_candidate.presentation.candidates[1].photo_id = photo;
        assert!(matches!(
            catalog.append_feedback_event(&duplicate_candidate),
            Err(CatalogError::InvalidFeedback(_))
        ));

        let mut missing_pairwise_candidate = pairwise_event(
            "missing-pairwise-candidate",
            LearningScope::Global,
            photo,
            PhotoId::new_v7(),
        );
        missing_pairwise_candidate.presentation.candidates.pop();
        assert!(matches!(
            catalog.append_feedback_event(&missing_pairwise_candidate),
            Err(CatalogError::InvalidFeedback(_))
        ));

        let invalid_rating = NewFeedbackEvent {
            event_id: "invalid-rating".into(),
            occurred_at_unix_ms: 1,
            scope: LearningScope::Global,
            presentation: presentation(vec![]),
            action: FeedbackAction::RatingChanged {
                photo_id: photo,
                before: Some(5),
                after: Some(6),
            },
        };
        assert!(matches!(
            catalog.append_feedback_event(&invalid_rating),
            Err(CatalogError::InvalidFeedback(_))
        ));

        let invalid_residual = NewFeedbackEvent {
            event_id: "invalid-residual".into(),
            occurred_at_unix_ms: 1,
            scope: LearningScope::Global,
            presentation: presentation(vec![]),
            action: FeedbackAction::DevelopProposalEdited {
                proposal_id: "proposal-1".into(),
                suggested_recipe: RecipeCommitId::new_v7(),
                final_recipe: RecipeCommitId::new_v7(),
                parameter_residuals: BTreeMap::from([("exposure".into(), f64::NAN)]),
            },
        };
        assert!(matches!(
            catalog.append_feedback_event(&invalid_residual),
            Err(CatalogError::InvalidFeedback(_))
        ));

        let unknown_photo =
            exported_event("unknown-photo", LearningScope::Global, PhotoId::new_v7());
        assert!(matches!(
            catalog.append_feedback_event(&unknown_photo),
            Err(CatalogError::PhotoNotFound(_))
        ));
        assert!(
            catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read empty feedback")
                .events
                .is_empty()
        );
    }

    #[test]
    fn presented_visual_representation_must_belong_to_its_candidate_photo() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let left = register_source(&mut catalog, 1);
        let right = register_source(&mut catalog, 2);
        let mut request = pairwise_event(
            "cross-owned-visual",
            LearningScope::Global,
            left.photo_id,
            right.photo_id,
        );
        request.presentation.candidates[0].visual = Some(presented_visual(right.representation_id));
        request.presentation.candidates[1].visual = Some(presented_visual(right.representation_id));

        assert!(matches!(
            catalog.append_feedback_event(&request),
            Err(CatalogError::FeedbackVisualRepresentationOwnerMismatch {
                photo_id,
                representation_id,
            }) if photo_id == left.photo_id && representation_id == right.representation_id
        ));
        assert!(
            catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read empty feedback")
                .events
                .is_empty()
        );
    }

    #[test]
    fn presented_visual_history_survives_rebuildable_cache_deletion() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let registered = register_source(&mut catalog, 1);
        let source = RepresentationFingerprint {
            byte_len: 42,
            modified_at_ms: Some(1),
        };
        assert_eq!(
            catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id: registered.representation_id,
                    expected_source: source,
                    artifact: CachedArtifact {
                        role: CachedArtifactRole::EmbeddedPreview,
                        variant_key: "embedded-0".into(),
                        generator_id: "test-preview-extractor".into(),
                        generator_version: "1".into(),
                        provider_preview_id: Some(0),
                        blob_algorithm: "blake3".into(),
                        blob_digest: [7; 32],
                        blob_byte_len: 2_048,
                        codec: PreviewCodec::Jpeg,
                        byte_order: PreviewByteOrder::NotApplicable,
                        dimensions: ImageDimensions {
                            width: 1_920,
                            height: 1_280,
                        },
                        bits_per_channel: 8,
                        channels: 3,
                        created_at_ms: 1_700_000_000_100,
                    },
                })
                .expect("record cache artifact"),
            RecordCachedArtifactStatus::Recorded
        );
        let visual = presented_visual(registered.representation_id);
        let event = NewFeedbackEvent {
            event_id: "visual-survives-cache".into(),
            occurred_at_unix_ms: 1_700_000_001_000,
            scope: LearningScope::Global,
            presentation: presentation(vec![PresentedCandidate {
                photo_id: registered.photo_id,
                position: 0,
                visible_fraction: UnitInterval::ONE,
                inspected_at_one_to_one: false,
                feature: None,
                visual: Some(visual.clone()),
            }]),
            action: FeedbackAction::Exported {
                photo_id: registered.photo_id,
            },
        };
        catalog
            .append_feedback_event(&event)
            .expect("append visual evidence");

        let cached = catalog
            .preferred_cached_artifact(registered.representation_id)
            .expect("load preferred cache artifact")
            .expect("cached artifact");
        assert_eq!(
            catalog
                .invalidate_cached_artifact(&cached)
                .expect("invalidate cache artifact"),
            InvalidateCachedArtifactStatus::Invalidated
        );
        assert!(
            catalog
                .cached_artifacts(registered.representation_id)
                .expect("read deleted cache rows")
                .is_empty()
        );

        let page = catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read historical evidence");
        assert_eq!(
            page.events[0].presentation.candidates[0].visual,
            Some(visual)
        );
    }

    #[test]
    fn forgetting_is_append_only_and_never_deletes_the_source_event() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo = register_photo(&mut catalog, 1);
        catalog
            .append_feedback_event(&exported_event(
                "event-to-forget",
                LearningScope::Global,
                photo,
            ))
            .expect("append source event");
        let forget = NewFeedbackForgetFact {
            fact_id: "forget-1".into(),
            target_event_id: "event-to-forget".into(),
            occurred_at_unix_ms: 1_700_000_002_000,
            reason: Some("user requested retraining exclusion".into()),
        };
        let stored_forget = catalog
            .append_feedback_forget_fact(&forget)
            .expect("append forget fact");
        assert_eq!(stored_forget.sequence, 1);
        assert!(matches!(
            catalog.append_feedback_forget_fact(&forget),
            Err(CatalogError::FeedbackForgetFactAlreadyExists(id)) if id == "forget-1"
        ));

        let forgotten = catalog
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read forgotten ids");
        assert_eq!(forgotten, BTreeSet::from(["event-to-forget".into()]));
        assert_eq!(
            catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read preserved source")
                .events
                .len(),
            1
        );
        let delete_error = catalog.connection.execute(
            "DELETE FROM ai_feedback_events WHERE event_id = ?1",
            ["event-to-forget"],
        );
        let update_error = catalog.connection.execute(
            "UPDATE ai_feedback_events SET occurred_at_ms = 0 WHERE event_id = ?1",
            ["event-to-forget"],
        );
        let delete_forget_error = catalog.connection.execute(
            "DELETE FROM ai_feedback_forget_facts WHERE fact_id = ?1",
            ["forget-1"],
        );
        let update_forget_error = catalog.connection.execute(
            "UPDATE ai_feedback_forget_facts SET occurred_at_ms = 0 WHERE fact_id = ?1",
            ["forget-1"],
        );
        assert!(delete_error.is_err());
        assert!(update_error.is_err());
        assert!(delete_forget_error.is_err());
        assert!(update_forget_error.is_err());
    }

    #[test]
    fn reads_verify_digest_canonical_json_and_indexed_columns() {
        let digest_catalog = catalog_with_exported_event("digest-event");
        digest_catalog
            .connection
            .execute_batch("DROP TRIGGER ai_feedback_events_no_update")
            .expect("disable update guard in corruption fixture");
        digest_catalog
            .connection
            .execute(
                "UPDATE ai_feedback_events SET event_digest = zeroblob(32)
                 WHERE event_id = ?1",
                ["digest-event"],
            )
            .expect("corrupt digest fixture");
        assert!(matches!(
            digest_catalog.feedback_events_after(&LearningScope::Global, 0, 10),
            Err(CatalogError::InvalidPersistedFeedback(
                "event JSON digest does not match"
            ))
        ));

        let canonical_catalog = catalog_with_exported_event("canonical-event");
        canonical_catalog
            .connection
            .execute_batch("DROP TRIGGER ai_feedback_events_no_update")
            .expect("disable update guard in canonical fixture");
        let canonical: String = canonical_catalog
            .connection
            .query_row(
                "SELECT event_json FROM ai_feedback_events WHERE event_id = ?1",
                ["canonical-event"],
                |row| row.get(0),
            )
            .expect("read canonical JSON");
        let non_canonical = format!(" {canonical}");
        let non_canonical_digest = blake3::hash(non_canonical.as_bytes());
        canonical_catalog
            .connection
            .execute(
                "UPDATE ai_feedback_events SET event_json = ?1, event_digest = ?2
                 WHERE event_id = ?3",
                params![
                    non_canonical,
                    non_canonical_digest.as_bytes().as_slice(),
                    "canonical-event",
                ],
            )
            .expect("corrupt canonical fixture");
        assert!(matches!(
            canonical_catalog.feedback_events_after(&LearningScope::Global, 0, 10),
            Err(CatalogError::InvalidPersistedFeedback(
                "event JSON is not canonical"
            ))
        ));

        let indexed_catalog = catalog_with_exported_event("indexed-event");
        indexed_catalog
            .connection
            .execute_batch("DROP TRIGGER ai_feedback_events_no_update")
            .expect("disable update guard in indexed fixture");
        indexed_catalog
            .connection
            .execute(
                "UPDATE ai_feedback_events SET occurred_at_ms = occurred_at_ms + 1
                 WHERE event_id = ?1",
                ["indexed-event"],
            )
            .expect("corrupt indexed fixture");
        assert!(matches!(
            indexed_catalog.feedback_events_after(&LearningScope::Global, 0, 10),
            Err(CatalogError::InvalidPersistedFeedback(
                "event JSON identity disagrees with indexed columns"
            ))
        ));
    }

    #[test]
    fn persisted_page_feeds_incremental_preference_batch_without_translation() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let left = register_photo(&mut catalog, 1);
        let right = register_photo(&mut catalog, 2);
        catalog
            .append_feedback_event(&pairwise_event(
                "pairwise-1",
                LearningScope::Global,
                left,
                right,
            ))
            .expect("append pairwise event");
        let page = catalog
            .feedback_events_after(&LearningScope::Global, 0, 100)
            .expect("read training page");
        let policy = IncrementalTrainingPolicy {
            scope: LearningScope::Global,
            learning_paused: false,
            after_sequence_exclusive: 0,
            maximum_examples: 100,
            forgotten_event_ids: catalog
                .forgotten_feedback_event_ids(&LearningScope::Global)
                .expect("read forgotten ids"),
        };
        let report = build_incremental_preference_batch(&page.events, &policy);
        assert_eq!(report.batch.examples.len(), 1);
        assert_eq!(report.batch.examples[0].event_id, "pairwise-1");
        assert_eq!(report.batch.examples[0].preferred_photo, left);
    }

    #[test]
    fn page_limit_is_strictly_bounded() {
        let catalog = Catalog::open_in_memory().expect("open catalog");
        assert!(matches!(
            catalog.feedback_events_after(&LearningScope::Global, 0, 0),
            Err(CatalogError::InvalidFeedbackPageLimit { .. })
        ));
        assert!(matches!(
            catalog.feedback_events_after(&LearningScope::Global, 0, MAX_FEEDBACK_PAGE_SIZE + 1,),
            Err(CatalogError::InvalidFeedbackPageLimit { .. })
        ));
    }
}
