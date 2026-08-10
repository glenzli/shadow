use std::{
    fs,
    path::Path,
    str::FromStr as _,
    sync::atomic::{AtomicU64, Ordering},
};

use rusqlite::{Connection, OptionalExtension as _, TransactionBehavior, params};
use shadow_ai::{
    ClassificationReviewSuggestion, ImageUnderstandingProvenance, SemanticImageAnalysis,
};
use shadow_catalog::{LibraryPhotoCursor, LibraryPhotoCursorValue};
use shadow_domain::{PhotoId, RepresentationId};
use thiserror::Error;
use time::OffsetDateTime;

use super::ImageUnderstandingScanPolicy;

const STORE_SCHEMA_VERSION: i64 = 1;
const MAX_REVISION_BYTES: usize = 256;
static GENERATION_SEQUENCE: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImageUnderstandingRunStatus {
    Running,
    Paused,
    Complete,
    Failed,
}

impl ImageUnderstandingRunStatus {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Running => "running",
            Self::Paused => "paused",
            Self::Complete => "complete",
            Self::Failed => "failed",
        }
    }

    fn parse(value: &str) -> Result<Self, ImageUnderstandingStoreError> {
        match value {
            "running" => Ok(Self::Running),
            "paused" => Ok(Self::Paused),
            "complete" => Ok(Self::Complete),
            "failed" => Ok(Self::Failed),
            _ => Err(ImageUnderstandingStoreError::CorruptState),
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImageUnderstandingProposalDisposition {
    Suggested,
    AutoApplied,
    Accepted,
    Dismissed,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ClassificationReviewProposalDisposition {
    Suggested,
    Accepted,
    Dismissed,
}

impl ClassificationReviewProposalDisposition {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Suggested => "suggested",
            Self::Accepted => "accepted",
            Self::Dismissed => "dismissed",
        }
    }

    fn parse(value: &str) -> Result<Self, ImageUnderstandingStoreError> {
        match value {
            "suggested" => Ok(Self::Suggested),
            "accepted" => Ok(Self::Accepted),
            "dismissed" => Ok(Self::Dismissed),
            _ => Err(ImageUnderstandingStoreError::CorruptState),
        }
    }
}

impl ImageUnderstandingProposalDisposition {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Suggested => "suggested",
            Self::AutoApplied => "auto_applied",
            Self::Accepted => "accepted",
            Self::Dismissed => "dismissed",
        }
    }

    fn parse(value: &str) -> Result<Self, ImageUnderstandingStoreError> {
        match value {
            "suggested" => Ok(Self::Suggested),
            "auto_applied" => Ok(Self::AutoApplied),
            "accepted" => Ok(Self::Accepted),
            "dismissed" => Ok(Self::Dismissed),
            _ => Err(ImageUnderstandingStoreError::CorruptState),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImageUnderstandingRunSnapshot {
    pub policy_revision: String,
    pub generation: String,
    pub status: ImageUnderstandingRunStatus,
    pub branch_count: usize,
    pub completed_branches: usize,
    pub processed_photos: u64,
    pub total_photos: u64,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ImageUnderstandingProposal {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub source_revision: String,
    pub analysis: SemanticImageAnalysis,
    pub provenance: ImageUnderstandingProvenance,
    pub disposition: ImageUnderstandingProposalDisposition,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ClassificationReviewProposal {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub source_revision: String,
    pub taxonomy_revision: String,
    pub suggestion: ClassificationReviewSuggestion,
    pub provenance: ImageUnderstandingProvenance,
    pub disposition: ClassificationReviewProposalDisposition,
    pub updated_at_ms: i64,
}

#[derive(Debug)]
pub(super) struct ImageUnderstandingStore {
    connection: Connection,
}

pub(super) struct StoreProposal<'a> {
    pub generation: &'a str,
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub expected_source_revision: &'a str,
    pub current_source_revision: &'a str,
    pub analysis: &'a SemanticImageAnalysis,
    pub provenance: &'a ImageUnderstandingProvenance,
    pub disposition: ImageUnderstandingProposalDisposition,
}

pub(super) struct StoreClassificationReview<'a> {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub expected_source_revision: &'a str,
    pub current_source_revision: &'a str,
    pub taxonomy_revision: &'a str,
    pub suggestion: &'a ClassificationReviewSuggestion,
    pub provenance: &'a ImageUnderstandingProvenance,
}

impl ImageUnderstandingStore {
    /// Opens the rebuildable, device-local image-understanding sidecar.
    ///
    /// # Errors
    ///
    /// Returns filesystem, `SQLite`, or unsupported-schema errors.
    pub(super) fn open(cache_root: impl AsRef<Path>) -> Result<Self, ImageUnderstandingStoreError> {
        let root = cache_root.as_ref().join("image-understanding");
        fs::create_dir_all(&root)?;
        let connection = Connection::open(root.join("proposals-v1.sqlite3"))?;
        connection.execute_batch(
            "PRAGMA journal_mode=WAL;
             PRAGMA synchronous=NORMAL;
             CREATE TABLE IF NOT EXISTS schema_meta(version INTEGER NOT NULL);
             CREATE TABLE IF NOT EXISTS run_state(
                 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),
                 policy_revision TEXT NOT NULL,
                 generation TEXT NOT NULL,
                 status TEXT NOT NULL,
                 branch_count INTEGER NOT NULL,
                 processed_photos INTEGER NOT NULL,
                 total_photos INTEGER NOT NULL,
                 updated_at_ms INTEGER NOT NULL
             );
             CREATE TABLE IF NOT EXISTS branch_checkpoints(
                 generation TEXT NOT NULL,
                 branch_index INTEGER NOT NULL,
                 cursor_kind TEXT,
                 cursor_text TEXT,
                 cursor_integer INTEGER,
                 cursor_photo_id TEXT,
                 complete INTEGER NOT NULL,
                 PRIMARY KEY(generation, branch_index)
             );
             CREATE TABLE IF NOT EXISTS processed_photos(
                 generation TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 PRIMARY KEY(generation, photo_id, representation_id, source_revision)
             );
             CREATE TABLE IF NOT EXISTS proposals(
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 analysis_json TEXT NOT NULL,
                 provenance_json TEXT NOT NULL,
                 disposition TEXT NOT NULL,
                 updated_at_ms INTEGER NOT NULL,
                 PRIMARY KEY(photo_id, representation_id)
             );
             CREATE INDEX IF NOT EXISTS proposals_disposition
                 ON proposals(disposition, updated_at_ms);
             CREATE TABLE IF NOT EXISTS keyword_bindings(
                 concept_id TEXT PRIMARY KEY,
                 keyword_id TEXT NOT NULL,
                 display_label TEXT NOT NULL,
                 updated_at_ms INTEGER NOT NULL
             );
             CREATE TABLE IF NOT EXISTS classification_reviews(
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 taxonomy_revision TEXT NOT NULL,
                 suggestion_json TEXT NOT NULL,
                 provenance_json TEXT NOT NULL,
                 disposition TEXT NOT NULL,
                 updated_at_ms INTEGER NOT NULL,
                 PRIMARY KEY(photo_id, representation_id)
             );
             CREATE INDEX IF NOT EXISTS classification_reviews_disposition
                 ON classification_reviews(disposition, updated_at_ms);",
        )?;
        let version = connection
            .query_row("SELECT version FROM schema_meta LIMIT 1", [], |row| {
                row.get(0)
            })
            .optional()?;
        match version {
            None => {
                connection.execute(
                    "INSERT INTO schema_meta(version) VALUES (?1)",
                    [STORE_SCHEMA_VERSION],
                )?;
            }
            Some(STORE_SCHEMA_VERSION) => {}
            Some(_) => return Err(ImageUnderstandingStoreError::UnsupportedSchema),
        }
        Ok(Self { connection })
    }

    /// Starts a fresh generation while retaining previously generated
    /// proposals as rebuildable UI evidence until a replacement is committed.
    pub(super) fn start_run(
        &mut self,
        policy: ImageUnderstandingScanPolicy,
        total_photos: u64,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        let policy_revision = policy.revision();
        let generation = new_generation(&policy_revision);
        let branch_count = policy.catalog_filters().len();
        let updated_at_ms = now_ms();
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        transaction.execute(
            "INSERT INTO run_state(
                 singleton_id, policy_revision, generation, status, branch_count,
                 processed_photos, total_photos, updated_at_ms
             ) VALUES (1, ?1, ?2, 'running', ?3, 0, ?4, ?5)
             ON CONFLICT(singleton_id) DO UPDATE SET
                 policy_revision = excluded.policy_revision,
                 generation = excluded.generation,
                 status = excluded.status,
                 branch_count = excluded.branch_count,
                 processed_photos = 0,
                 total_photos = excluded.total_photos,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                policy_revision,
                generation,
                i64::try_from(branch_count)
                    .map_err(|_| ImageUnderstandingStoreError::InvalidRun)?,
                to_i64(total_photos)?,
                updated_at_ms
            ],
        )?;
        for branch_index in 0..branch_count {
            transaction.execute(
                "INSERT INTO branch_checkpoints(
                     generation, branch_index, cursor_kind, cursor_text,
                     cursor_integer, cursor_photo_id, complete
                 ) VALUES (?1, ?2, NULL, NULL, NULL, NULL, 0)",
                params![generation, i64::try_from(branch_index).unwrap_or(i64::MAX)],
            )?;
        }
        transaction.commit()?;
        self.snapshot_for_generation(&generation)
    }

    pub(super) fn snapshot(
        &self,
    ) -> Result<Option<ImageUnderstandingRunSnapshot>, ImageUnderstandingStoreError> {
        self.read_snapshot(None)
    }

    pub(super) fn resume_run(
        &self,
        policy: ImageUnderstandingScanPolicy,
        generation: &str,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        validate_revision(generation)?;
        let snapshot = self.snapshot_for_generation(generation)?;
        if snapshot.policy_revision != policy.revision() {
            return Err(ImageUnderstandingStoreError::PolicyChanged);
        }
        if matches!(snapshot.status, ImageUnderstandingRunStatus::Complete) {
            return Ok(snapshot);
        }
        self.transition(generation, ImageUnderstandingRunStatus::Running)?;
        self.snapshot_for_generation(generation)
    }

    pub(super) fn pause_run(
        &self,
        generation: &str,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        self.transition(generation, ImageUnderstandingRunStatus::Paused)?;
        self.snapshot_for_generation(generation)
    }

    pub(super) fn fail_run(
        &self,
        generation: &str,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        self.transition(generation, ImageUnderstandingRunStatus::Failed)?;
        self.snapshot_for_generation(generation)
    }

    pub(super) fn branch_cursor(
        &self,
        generation: &str,
        branch_index: usize,
    ) -> Result<Option<LibraryPhotoCursor>, ImageUnderstandingStoreError> {
        self.ensure_active_generation(generation)?;
        let row = self
            .connection
            .query_row(
                "SELECT cursor_kind, cursor_text, cursor_integer, cursor_photo_id
                 FROM branch_checkpoints
                 WHERE generation = ?1 AND branch_index = ?2",
                params![generation, to_i64_usize(branch_index)?],
                |row| {
                    Ok((
                        row.get::<_, Option<String>>(0)?,
                        row.get::<_, Option<String>>(1)?,
                        row.get::<_, Option<i64>>(2)?,
                        row.get::<_, Option<String>>(3)?,
                    ))
                },
            )
            .optional()?
            .ok_or(ImageUnderstandingStoreError::InvalidBranch)?;
        decode_cursor(row)
    }

    pub(super) fn branch_complete(
        &self,
        generation: &str,
        branch_index: usize,
    ) -> Result<bool, ImageUnderstandingStoreError> {
        self.ensure_active_generation(generation)?;
        self.connection
            .query_row(
                "SELECT complete FROM branch_checkpoints
                 WHERE generation = ?1 AND branch_index = ?2",
                params![generation, to_i64_usize(branch_index)?],
                |row| row.get::<_, bool>(0),
            )
            .optional()?
            .ok_or(ImageUnderstandingStoreError::InvalidBranch)
    }

    pub(super) fn checkpoint_branch(
        &self,
        generation: &str,
        branch_index: usize,
        next_cursor: Option<&LibraryPhotoCursor>,
        complete: bool,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        self.ensure_running_generation(generation)?;
        let (kind, text, integer, photo_id) = encode_cursor(next_cursor);
        let changed = self.connection.execute(
            "UPDATE branch_checkpoints SET
                 cursor_kind = ?3, cursor_text = ?4, cursor_integer = ?5,
                 cursor_photo_id = ?6, complete = ?7
             WHERE generation = ?1 AND branch_index = ?2",
            params![
                generation,
                to_i64_usize(branch_index)?,
                kind,
                text,
                integer,
                photo_id,
                complete
            ],
        )?;
        if changed != 1 {
            return Err(ImageUnderstandingStoreError::InvalidBranch);
        }
        self.finish_if_all_branches_complete(generation)?;
        self.snapshot_for_generation(generation)
    }

    pub(super) fn was_processed(
        &self,
        generation: &str,
        photo_id: PhotoId,
        representation_id: RepresentationId,
        source_revision: &str,
    ) -> Result<bool, ImageUnderstandingStoreError> {
        self.ensure_active_generation(generation)?;
        Ok(self.connection.query_row(
            "SELECT EXISTS(
                 SELECT 1 FROM processed_photos
                 WHERE generation = ?1 AND photo_id = ?2
                   AND representation_id = ?3 AND source_revision = ?4
             )",
            params![
                generation,
                photo_id.to_string(),
                representation_id.to_string(),
                source_revision
            ],
            |row| row.get(0),
        )?)
    }

    /// Atomically records one exact-revision proposal and its processed marker.
    /// The current source revision is supplied by the caller after inference;
    /// a mismatch rejects stale output before any durable write.
    pub(super) fn store_proposal(
        &mut self,
        proposal: &StoreProposal<'_>,
    ) -> Result<(), ImageUnderstandingStoreError> {
        self.ensure_running_generation(proposal.generation)?;
        validate_revision(proposal.expected_source_revision)?;
        if proposal.expected_source_revision != proposal.current_source_revision {
            return Err(ImageUnderstandingStoreError::StaleSourceRevision);
        }
        let analysis_json = serde_json::to_string(proposal.analysis)?;
        let provenance_json = serde_json::to_string(proposal.provenance)?;
        let updated_at_ms = now_ms();
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let inserted = transaction.execute(
            "INSERT OR IGNORE INTO processed_photos(
                 generation, photo_id, representation_id, source_revision
             ) VALUES (?1, ?2, ?3, ?4)",
            params![
                proposal.generation,
                proposal.photo_id.to_string(),
                proposal.representation_id.to_string(),
                proposal.expected_source_revision
            ],
        )?;
        transaction.execute(
            "INSERT INTO proposals(
                 photo_id, representation_id, source_revision, analysis_json,
                 provenance_json, disposition, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
             ON CONFLICT(photo_id, representation_id) DO UPDATE SET
                 source_revision = excluded.source_revision,
                 analysis_json = excluded.analysis_json,
                 provenance_json = excluded.provenance_json,
                 disposition = excluded.disposition,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                proposal.photo_id.to_string(),
                proposal.representation_id.to_string(),
                proposal.expected_source_revision,
                analysis_json,
                provenance_json,
                proposal.disposition.as_str(),
                updated_at_ms
            ],
        )?;
        if inserted == 1 {
            transaction.execute(
                "UPDATE run_state SET
                     processed_photos = processed_photos + 1,
                     updated_at_ms = ?2
                 WHERE singleton_id = 1 AND generation = ?1",
                params![proposal.generation, updated_at_ms],
            )?;
        }
        transaction.commit()?;
        Ok(())
    }

    pub(super) fn proposal(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<Option<ImageUnderstandingProposal>, ImageUnderstandingStoreError> {
        let stored = self
            .connection
            .query_row(
                "SELECT source_revision, analysis_json, provenance_json,
                        disposition, updated_at_ms
                 FROM proposals WHERE photo_id = ?1 AND representation_id = ?2",
                params![photo_id.to_string(), representation_id.to_string()],
                |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                        row.get::<_, String>(3)?,
                        row.get::<_, i64>(4)?,
                    ))
                },
            )
            .optional()?;
        stored
            .map(
                |(source_revision, analysis_json, provenance_json, disposition, updated_at_ms)| {
                    Ok(ImageUnderstandingProposal {
                        photo_id,
                        representation_id,
                        source_revision,
                        analysis: serde_json::from_str(&analysis_json)?,
                        provenance: serde_json::from_str(&provenance_json)?,
                        disposition: ImageUnderstandingProposalDisposition::parse(&disposition)?,
                        updated_at_ms,
                    })
                },
            )
            .transpose()
    }

    pub(super) fn set_proposal_disposition(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
        expected_source_revision: &str,
        disposition: ImageUnderstandingProposalDisposition,
    ) -> Result<bool, ImageUnderstandingStoreError> {
        validate_revision(expected_source_revision)?;
        Ok(self.connection.execute(
            "UPDATE proposals SET disposition = ?4, updated_at_ms = ?5
             WHERE photo_id = ?1 AND representation_id = ?2 AND source_revision = ?3",
            params![
                photo_id.to_string(),
                representation_id.to_string(),
                expected_source_revision,
                disposition.as_str(),
                now_ms()
            ],
        )? == 1)
    }

    pub(super) fn store_classification_review(
        &self,
        review: &StoreClassificationReview<'_>,
    ) -> Result<(), ImageUnderstandingStoreError> {
        validate_revision(review.expected_source_revision)?;
        validate_revision(review.taxonomy_revision)?;
        if review.expected_source_revision != review.current_source_revision {
            return Err(ImageUnderstandingStoreError::StaleSourceRevision);
        }
        self.connection.execute(
            "INSERT INTO classification_reviews(
                 photo_id, representation_id, source_revision, taxonomy_revision,
                 suggestion_json, provenance_json, disposition, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'suggested', ?7)
             ON CONFLICT(photo_id, representation_id) DO UPDATE SET
                 source_revision = excluded.source_revision,
                 taxonomy_revision = excluded.taxonomy_revision,
                 suggestion_json = excluded.suggestion_json,
                 provenance_json = excluded.provenance_json,
                 disposition = excluded.disposition,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                review.photo_id.to_string(),
                review.representation_id.to_string(),
                review.expected_source_revision,
                review.taxonomy_revision,
                serde_json::to_string(review.suggestion)?,
                serde_json::to_string(review.provenance)?,
                now_ms()
            ],
        )?;
        Ok(())
    }

    pub(super) fn classification_review(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<Option<ClassificationReviewProposal>, ImageUnderstandingStoreError> {
        let stored = self
            .connection
            .query_row(
                "SELECT source_revision, taxonomy_revision, suggestion_json,
                        provenance_json, disposition, updated_at_ms
                 FROM classification_reviews
                 WHERE photo_id = ?1 AND representation_id = ?2",
                params![photo_id.to_string(), representation_id.to_string()],
                |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                        row.get::<_, String>(3)?,
                        row.get::<_, String>(4)?,
                        row.get::<_, i64>(5)?,
                    ))
                },
            )
            .optional()?;
        stored
            .map(
                |(
                    source_revision,
                    taxonomy_revision,
                    suggestion_json,
                    provenance_json,
                    disposition,
                    updated_at_ms,
                )| {
                    Ok(ClassificationReviewProposal {
                        photo_id,
                        representation_id,
                        source_revision,
                        taxonomy_revision,
                        suggestion: serde_json::from_str(&suggestion_json)?,
                        provenance: serde_json::from_str(&provenance_json)?,
                        disposition: ClassificationReviewProposalDisposition::parse(&disposition)?,
                        updated_at_ms,
                    })
                },
            )
            .transpose()
    }

    pub(super) fn set_classification_review_disposition(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
        expected_source_revision: &str,
        disposition: ClassificationReviewProposalDisposition,
    ) -> Result<bool, ImageUnderstandingStoreError> {
        validate_revision(expected_source_revision)?;
        Ok(self.connection.execute(
            "UPDATE classification_reviews
             SET disposition = ?4, updated_at_ms = ?5
             WHERE photo_id = ?1 AND representation_id = ?2 AND source_revision = ?3",
            params![
                photo_id.to_string(),
                representation_id.to_string(),
                expected_source_revision,
                disposition.as_str(),
                now_ms()
            ],
        )? == 1)
    }

    pub(super) fn keyword_binding(
        &self,
        concept_id: &str,
    ) -> Result<Option<shadow_domain::KeywordId>, ImageUnderstandingStoreError> {
        validate_revision(concept_id)?;
        self.connection
            .query_row(
                "SELECT keyword_id FROM keyword_bindings WHERE concept_id = ?1",
                [concept_id],
                |row| row.get::<_, String>(0),
            )
            .optional()?
            .map(|value| {
                shadow_domain::KeywordId::from_str(&value)
                    .map_err(|_| ImageUnderstandingStoreError::CorruptState)
            })
            .transpose()
    }

    pub(super) fn bind_keyword(
        &self,
        concept_id: &str,
        keyword_id: shadow_domain::KeywordId,
        display_label: &str,
    ) -> Result<(), ImageUnderstandingStoreError> {
        validate_revision(concept_id)?;
        self.connection.execute(
            "INSERT INTO keyword_bindings(
                 concept_id, keyword_id, display_label, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4)
             ON CONFLICT(concept_id) DO UPDATE SET
                 keyword_id = excluded.keyword_id,
                 display_label = excluded.display_label,
                 updated_at_ms = excluded.updated_at_ms",
            params![concept_id, keyword_id.to_string(), display_label, now_ms()],
        )?;
        Ok(())
    }

    fn transition(
        &self,
        generation: &str,
        status: ImageUnderstandingRunStatus,
    ) -> Result<(), ImageUnderstandingStoreError> {
        validate_revision(generation)?;
        let changed = self.connection.execute(
            "UPDATE run_state SET status = ?2, updated_at_ms = ?3
             WHERE singleton_id = 1 AND generation = ?1",
            params![generation, status.as_str(), now_ms()],
        )?;
        if changed != 1 {
            return Err(ImageUnderstandingStoreError::StaleGeneration);
        }
        Ok(())
    }

    fn finish_if_all_branches_complete(
        &self,
        generation: &str,
    ) -> Result<(), ImageUnderstandingStoreError> {
        let incomplete: i64 = self.connection.query_row(
            "SELECT COUNT(*) FROM branch_checkpoints
             WHERE generation = ?1 AND complete = 0",
            [generation],
            |row| row.get(0),
        )?;
        if incomplete == 0 {
            self.transition(generation, ImageUnderstandingRunStatus::Complete)?;
        }
        Ok(())
    }

    fn ensure_active_generation(
        &self,
        generation: &str,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        validate_revision(generation)?;
        self.snapshot_for_generation(generation)
    }

    fn ensure_running_generation(
        &self,
        generation: &str,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        let snapshot = self.ensure_active_generation(generation)?;
        if snapshot.status != ImageUnderstandingRunStatus::Running {
            return Err(ImageUnderstandingStoreError::RunNotRunning);
        }
        Ok(snapshot)
    }

    fn snapshot_for_generation(
        &self,
        generation: &str,
    ) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
        self.read_snapshot(Some(generation))?
            .ok_or(ImageUnderstandingStoreError::StaleGeneration)
    }

    fn read_snapshot(
        &self,
        generation: Option<&str>,
    ) -> Result<Option<ImageUnderstandingRunSnapshot>, ImageUnderstandingStoreError> {
        let sql = if generation.is_some() {
            "SELECT policy_revision, generation, status, branch_count,
                    processed_photos, total_photos, updated_at_ms
             FROM run_state WHERE singleton_id = 1 AND generation = ?1"
        } else {
            "SELECT policy_revision, generation, status, branch_count,
                    processed_photos, total_photos, updated_at_ms
             FROM run_state WHERE singleton_id = 1 AND ?1 IS NULL"
        };
        let row = self
            .connection
            .query_row(sql, [generation], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, i64>(3)?,
                    row.get::<_, i64>(4)?,
                    row.get::<_, i64>(5)?,
                    row.get::<_, i64>(6)?,
                ))
            })
            .optional()?;
        row.map(
            |(
                policy_revision,
                generation,
                status,
                branch_count,
                processed_photos,
                total_photos,
                updated_at_ms,
            )| {
                let completed_branches = self.connection.query_row(
                    "SELECT COUNT(*) FROM branch_checkpoints
                     WHERE generation = ?1 AND complete = 1",
                    [&generation],
                    |row| row.get::<_, i64>(0),
                )?;
                Ok(ImageUnderstandingRunSnapshot {
                    policy_revision,
                    generation,
                    status: ImageUnderstandingRunStatus::parse(&status)?,
                    branch_count: to_usize(branch_count)?,
                    completed_branches: to_usize(completed_branches)?,
                    processed_photos: to_u64(processed_photos)?,
                    total_photos: to_u64(total_photos)?,
                    updated_at_ms,
                })
            },
        )
        .transpose()
    }
}

fn encode_cursor(
    cursor: Option<&LibraryPhotoCursor>,
) -> (
    Option<&'static str>,
    Option<String>,
    Option<i64>,
    Option<String>,
) {
    let Some(cursor) = cursor else {
        return (None, None, None, None);
    };
    match &cursor.value {
        LibraryPhotoCursorValue::CaptureTime(Some(value)) => (
            Some("capture_time"),
            None,
            Some(*value),
            Some(cursor.photo_id.to_string()),
        ),
        LibraryPhotoCursorValue::CaptureTime(None) => (
            Some("capture_time_missing"),
            None,
            None,
            Some(cursor.photo_id.to_string()),
        ),
        LibraryPhotoCursorValue::FileName(value) => (
            Some("file_name"),
            Some(value.clone()),
            None,
            Some(cursor.photo_id.to_string()),
        ),
    }
}

fn decode_cursor(
    row: (Option<String>, Option<String>, Option<i64>, Option<String>),
) -> Result<Option<LibraryPhotoCursor>, ImageUnderstandingStoreError> {
    let (kind, text, integer, photo_id) = row;
    let Some(kind) = kind else {
        if text.is_none() && integer.is_none() && photo_id.is_none() {
            return Ok(None);
        }
        return Err(ImageUnderstandingStoreError::CorruptState);
    };
    let photo_id = photo_id
        .ok_or(ImageUnderstandingStoreError::CorruptState)
        .and_then(|value| {
            PhotoId::from_str(&value).map_err(|_| ImageUnderstandingStoreError::CorruptState)
        })?;
    let value = match kind.as_str() {
        "capture_time" if text.is_none() && integer.is_some() => {
            LibraryPhotoCursorValue::CaptureTime(integer)
        }
        "capture_time_missing" if text.is_none() && integer.is_none() => {
            LibraryPhotoCursorValue::CaptureTime(None)
        }
        "file_name" if text.is_some() && integer.is_none() => {
            LibraryPhotoCursorValue::FileName(text.unwrap_or_default())
        }
        _ => return Err(ImageUnderstandingStoreError::CorruptState),
    };
    Ok(Some(LibraryPhotoCursor { value, photo_id }))
}

fn validate_revision(value: &str) -> Result<(), ImageUnderstandingStoreError> {
    if value.trim().is_empty()
        || value.len() > MAX_REVISION_BYTES
        || value.chars().any(char::is_control)
    {
        return Err(ImageUnderstandingStoreError::InvalidRevision);
    }
    Ok(())
}

fn new_generation(policy_revision: &str) -> String {
    let timestamp = now_ms();
    let sequence = GENERATION_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let digest = blake3::hash(format!("{policy_revision}\0{timestamp}\0{sequence}").as_bytes());
    format!("iu_{}", &digest.to_hex()[..24])
}

fn now_ms() -> i64 {
    let milliseconds = OffsetDateTime::now_utc().unix_timestamp_nanos() / 1_000_000;
    i64::try_from(milliseconds).unwrap_or(i64::MAX)
}

fn to_i64(value: u64) -> Result<i64, ImageUnderstandingStoreError> {
    i64::try_from(value).map_err(|_| ImageUnderstandingStoreError::InvalidRun)
}

fn to_i64_usize(value: usize) -> Result<i64, ImageUnderstandingStoreError> {
    i64::try_from(value).map_err(|_| ImageUnderstandingStoreError::InvalidRun)
}

fn to_u64(value: i64) -> Result<u64, ImageUnderstandingStoreError> {
    u64::try_from(value).map_err(|_| ImageUnderstandingStoreError::CorruptState)
}

fn to_usize(value: i64) -> Result<usize, ImageUnderstandingStoreError> {
    usize::try_from(value).map_err(|_| ImageUnderstandingStoreError::CorruptState)
}

#[derive(Debug, Error)]
pub enum ImageUnderstandingStoreError {
    #[error("image-understanding sidecar filesystem error: {0}")]
    Io(#[from] std::io::Error),
    #[error("image-understanding sidecar database error: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error("image-understanding proposal serialization error: {0}")]
    Serialization(#[from] serde_json::Error),
    #[error("image-understanding sidecar schema is unsupported")]
    UnsupportedSchema,
    #[error("image-understanding revision is invalid")]
    InvalidRevision,
    #[error("image-understanding run parameters are invalid")]
    InvalidRun,
    #[error("image-understanding run generation is stale")]
    StaleGeneration,
    #[error("image-understanding scan policy changed")]
    PolicyChanged,
    #[error("image-understanding run is not running")]
    RunNotRunning,
    #[error("image-understanding branch is invalid")]
    InvalidBranch,
    #[error("image-understanding output belongs to a stale source revision")]
    StaleSourceRevision,
    #[error("image-understanding sidecar state is corrupt")]
    CorruptState,
}
