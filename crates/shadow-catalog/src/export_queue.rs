//! Durable export presets, immutable render snapshots, and resumable queue state.
//!
//! Exporting is intentionally a catalog concern rather than a transient UI task.
//! A queued item owns an immutable recipe commit, source identity, render plan,
//! output target, and output-settings snapshot. Renderers may therefore retry or
//! resume work without accidentally exporting a later working edit.

use std::fmt;

use rusqlite::{OptionalExtension, Transaction, params, types::Type};
use serde::{Deserialize, Serialize};
use shadow_domain::{
    AssetLocation, EditCommitId, EntityId, PhotoId, Platform, RecipeCommitId, RepresentationId,
};
use uuid::Uuid;

use crate::{
    Catalog, CatalogError,
    cache_artifact::{digest, non_negative_u64},
    row_codec::read_id,
};

/// Schema-v1 component for durable export work. The numeric catalog schema is
/// deliberately still v1 while development is unstable; changing this shape
/// changes the v1 identity marker and requires a development-catalog reset.
pub(crate) const SCHEMA_V1_EXPORT_QUEUE: &str = r"
CREATE TABLE export_presets (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    name            TEXT NOT NULL CHECK (length(trim(name)) BETWEEN 1 AND 256),
    created_at_ms   INTEGER NOT NULL,
    archived_at_ms  INTEGER,
    UNIQUE (name COLLATE NOCASE)
) STRICT;

CREATE TABLE export_preset_revisions (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    preset_id       BLOB NOT NULL CHECK (length(preset_id) = 16),
    revision        INTEGER NOT NULL CHECK (revision > 0),
    settings_json   TEXT NOT NULL CHECK (json_valid(settings_json)),
    settings_digest BLOB NOT NULL CHECK (length(settings_digest) = 32),
    created_at_ms   INTEGER NOT NULL,
    UNIQUE (preset_id, revision),
    FOREIGN KEY (preset_id) REFERENCES export_presets(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_preset_revisions_preset_time_idx
    ON export_preset_revisions(preset_id, revision DESC);

CREATE TRIGGER export_preset_revisions_no_update
BEFORE UPDATE ON export_preset_revisions
BEGIN
    SELECT RAISE(ABORT, 'export preset revisions are immutable');
END;

CREATE TRIGGER export_preset_revisions_no_delete
BEFORE DELETE ON export_preset_revisions
BEGIN
    SELECT RAISE(ABORT, 'export preset revisions are immutable');
END;

CREATE TABLE export_jobs (
    id                  BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    state               TEXT NOT NULL
        CHECK (state IN ('queued', 'running', 'paused_conflict', 'cancelled',
                         'failed', 'completed', 'interrupted')),
    preset_revision_id  BLOB CHECK (preset_revision_id IS NULL OR length(preset_revision_id) = 16),
    settings_json       TEXT NOT NULL CHECK (json_valid(settings_json)),
    settings_digest     BLOB NOT NULL CHECK (length(settings_digest) = 32),
    created_at_ms       INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,
    started_at_ms       INTEGER,
    finished_at_ms      INTEGER,
    last_error_code     TEXT,
    last_error_message  TEXT,
    last_error_retryable INTEGER CHECK (last_error_retryable IN (0, 1)),
    CHECK (
        (last_error_code IS NULL AND last_error_message IS NULL AND last_error_retryable IS NULL)
        OR
        (length(trim(last_error_code)) BETWEEN 1 AND 128
         AND length(trim(last_error_message)) BETWEEN 1 AND 8192
         AND last_error_retryable IN (0, 1))
    ),
    FOREIGN KEY (preset_revision_id) REFERENCES export_preset_revisions(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_jobs_state_updated_idx
    ON export_jobs(state, updated_at_ms DESC, id);

CREATE TRIGGER export_jobs_snapshot_immutable
BEFORE UPDATE OF preset_revision_id, settings_json, settings_digest ON export_jobs
BEGIN
    SELECT RAISE(ABORT, 'export job snapshot is immutable');
END;

CREATE TABLE export_items (
    id                      BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    job_id                  BLOB NOT NULL CHECK (length(job_id) = 16),
    position                INTEGER NOT NULL CHECK (position >= 0),
    photo_id                BLOB NOT NULL CHECK (length(photo_id) = 16),
    representation_id       BLOB NOT NULL CHECK (length(representation_id) = 16),
    recipe_commit_id        BLOB NOT NULL CHECK (length(recipe_commit_id) = 16),
    recipe_snapshot_digest  BLOB NOT NULL CHECK (length(recipe_snapshot_digest) = 32),
    edit_commit_id          BLOB CHECK (edit_commit_id IS NULL OR length(edit_commit_id) = 32),
    source_identity_json    TEXT NOT NULL CHECK (json_valid(source_identity_json)),
    source_identity_digest  BLOB NOT NULL CHECK (length(source_identity_digest) = 32),
    render_plan_json        TEXT NOT NULL CHECK (json_valid(render_plan_json)),
    render_plan_digest      BLOB NOT NULL CHECK (length(render_plan_digest) = 32),
    output_platform         TEXT NOT NULL,
    output_native_path      BLOB NOT NULL CHECK (length(output_native_path) > 0),
    output_display_path     TEXT NOT NULL CHECK (length(trim(output_display_path)) > 0),
    state                   TEXT NOT NULL
        CHECK (state IN ('queued', 'preparing', 'rendering', 'encoding', 'writing_temp',
                         'paused_conflict', 'cancelled', 'failed', 'completed', 'interrupted')),
    attempt_count           INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    created_at_ms           INTEGER NOT NULL,
    updated_at_ms           INTEGER NOT NULL,
    started_at_ms           INTEGER,
    finished_at_ms          INTEGER,
    error_code              TEXT,
    error_message           TEXT,
    error_retryable         INTEGER CHECK (error_retryable IN (0, 1)),
    CHECK (
        (error_code IS NULL AND error_message IS NULL AND error_retryable IS NULL)
        OR
        (length(trim(error_code)) BETWEEN 1 AND 128
         AND length(trim(error_message)) BETWEEN 1 AND 8192
         AND error_retryable IN (0, 1))
    ),
    UNIQUE (job_id, position),
    FOREIGN KEY (job_id) REFERENCES export_jobs(id) ON DELETE CASCADE,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE RESTRICT,
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE RESTRICT,
    FOREIGN KEY (recipe_commit_id, photo_id)
        REFERENCES recipe_commits(id, photo_id) ON DELETE RESTRICT,
    FOREIGN KEY (edit_commit_id) REFERENCES edit_repository_commits(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_items_worker_claim_idx
    ON export_items(state, created_at_ms, job_id, position);
CREATE INDEX export_items_job_position_idx
    ON export_items(job_id, position);
CREATE INDEX export_items_photo_idx
    ON export_items(photo_id, created_at_ms DESC);

CREATE TRIGGER export_items_snapshot_immutable
BEFORE UPDATE OF job_id, position, photo_id, representation_id, recipe_commit_id,
                 recipe_snapshot_digest, edit_commit_id, source_identity_json,
                 source_identity_digest, render_plan_json, render_plan_digest,
                 output_platform, output_native_path, output_display_path
ON export_items
BEGIN
    SELECT RAISE(ABORT, 'export item snapshot is immutable');
END;

CREATE TABLE export_output_receipts (
    id                  BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    item_id             BLOB NOT NULL UNIQUE CHECK (length(item_id) = 16),
    output_platform     TEXT NOT NULL,
    output_native_path  BLOB NOT NULL CHECK (length(output_native_path) > 0),
    output_display_path TEXT NOT NULL CHECK (length(trim(output_display_path)) > 0),
    output_format       TEXT NOT NULL CHECK (length(trim(output_format)) BETWEEN 1 AND 64),
    byte_len            INTEGER NOT NULL CHECK (byte_len >= 0),
    content_digest      BLOB CHECK (content_digest IS NULL OR length(content_digest) = 32),
    receipt_json        TEXT NOT NULL CHECK (json_valid(receipt_json)),
    created_at_ms       INTEGER NOT NULL,
    FOREIGN KEY (item_id) REFERENCES export_items(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_output_receipts_created_idx
    ON export_output_receipts(created_at_ms DESC, id);

CREATE TRIGGER export_output_receipts_no_update
BEFORE UPDATE ON export_output_receipts
BEGIN
    SELECT RAISE(ABORT, 'export output receipts are immutable');
END;

CREATE TRIGGER export_output_receipts_no_delete
BEFORE DELETE ON export_output_receipts
BEGIN
    SELECT RAISE(ABORT, 'export output receipts are immutable');
END;
";

macro_rules! export_id {
    ($name:ident) => {
        #[derive(
            Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize,
        )]
        #[serde(transparent)]
        pub struct $name(Uuid);

        impl EntityId for $name {
            fn new_v7() -> Self {
                Self(Uuid::now_v7())
            }

            fn from_uuid(value: Uuid) -> Self {
                Self(value)
            }

            fn as_uuid(self) -> Uuid {
                self.0
            }

            fn as_bytes(&self) -> &[u8; 16] {
                self.0.as_bytes()
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.fmt(formatter)
            }
        }
    };
}

export_id!(ExportPresetId);
export_id!(ExportPresetRevisionId);
export_id!(ExportJobId);
export_id!(ExportItemId);
export_id!(ExportOutputReceiptId);

/// User-visible export preset. Its mutable name is separate from the
/// immutable settings revisions referenced by export jobs.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ExportPresetRecord {
    pub id: ExportPresetId,
    pub name: String,
    pub created_at_ms: i64,
    pub archived_at_ms: Option<i64>,
}

/// Exact settings payload used by a saved preset revision.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ExportPresetRevisionRecord {
    pub id: ExportPresetRevisionId,
    pub preset_id: ExportPresetId,
    pub revision: u32,
    pub settings_json: String,
    pub settings_digest: [u8; 32],
    pub created_at_ms: i64,
}

/// Queue-level state derived from its item state, persisted for fast task
/// center queries and restart recovery.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum ExportJobState {
    Queued,
    Running,
    PausedConflict,
    Cancelled,
    Failed,
    Completed,
    Interrupted,
}

impl ExportJobState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Queued => "queued",
            Self::Running => "running",
            Self::PausedConflict => "paused_conflict",
            Self::Cancelled => "cancelled",
            Self::Failed => "failed",
            Self::Completed => "completed",
            Self::Interrupted => "interrupted",
        }
    }

    const fn is_terminal(self) -> bool {
        matches!(self, Self::Cancelled | Self::Failed | Self::Completed)
    }
}

/// Fine-grained durable state for one photo output.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum ExportItemState {
    Queued,
    Preparing,
    Rendering,
    Encoding,
    WritingTemp,
    PausedConflict,
    Cancelled,
    Failed,
    Completed,
    Interrupted,
}

impl ExportItemState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Queued => "queued",
            Self::Preparing => "preparing",
            Self::Rendering => "rendering",
            Self::Encoding => "encoding",
            Self::WritingTemp => "writing_temp",
            Self::PausedConflict => "paused_conflict",
            Self::Cancelled => "cancelled",
            Self::Failed => "failed",
            Self::Completed => "completed",
            Self::Interrupted => "interrupted",
        }
    }

    const fn is_terminal(self) -> bool {
        matches!(self, Self::Cancelled | Self::Failed | Self::Completed)
    }

    const fn allows_transition_to(self, next: Self) -> bool {
        match self {
            Self::Queued => matches!(next, Self::Preparing | Self::Cancelled),
            Self::Preparing => matches!(
                next,
                Self::Rendering | Self::Failed | Self::Cancelled | Self::Interrupted
            ),
            Self::Rendering => matches!(
                next,
                Self::Encoding | Self::Failed | Self::Cancelled | Self::Interrupted
            ),
            Self::Encoding => matches!(
                next,
                Self::WritingTemp | Self::Failed | Self::Cancelled | Self::Interrupted
            ),
            Self::WritingTemp => matches!(
                next,
                Self::Completed
                    | Self::PausedConflict
                    | Self::Failed
                    | Self::Cancelled
                    | Self::Interrupted
            ),
            Self::PausedConflict | Self::Failed | Self::Interrupted => {
                matches!(next, Self::Queued | Self::Cancelled)
            }
            Self::Cancelled | Self::Completed => false,
        }
    }
}

/// Durable failure details retained per item and projected onto the parent job.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ExportFailure {
    pub code: String,
    pub message: String,
    pub retryable: bool,
}

/// Immutable resolved output location and proof recorded after an atomic write.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct NewExportOutputReceipt {
    pub output: AssetLocation,
    pub output_format: String,
    pub byte_len: u64,
    pub content_digest: Option<[u8; 32]>,
    /// Encoder-specific reproducibility data, such as color space and metadata
    /// policy, serialized as one JSON object.
    pub receipt_json: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ExportOutputReceiptRecord {
    pub id: ExportOutputReceiptId,
    pub item_id: ExportItemId,
    pub output: AssetLocation,
    pub output_format: String,
    pub byte_len: u64,
    pub content_digest: Option<[u8; 32]>,
    pub receipt_json: String,
    pub created_at_ms: i64,
}

/// One frozen source/edit/output plan that will become a durable job item.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct NewExportItem {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    /// The auto-saved immutable working Recipe snapshot at enqueue time.
    pub recipe_commit_id: RecipeCommitId,
    pub recipe_snapshot_digest: [u8; 32],
    /// Optional Library-wide edit graph commit. When present it is a second
    /// immutable reference for shared nodes, masks, and styles.
    pub edit_commit_id: Option<EditCommitId>,
    /// Versioned source identity JSON, normally containing a whole-file hash
    /// and provider-specific payload/mosaic identities when available.
    pub source_identity_json: String,
    /// Frozen `RawDevelopmentPlan`, profiles, optics policy, quality tier, and
    /// renderer contract. It must not be re-read from mutable preferences.
    pub render_plan_json: String,
    pub output: AssetLocation,
}

/// The settings source copied into an export job at enqueue time.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum ExportSettingsSource {
    PresetRevision(ExportPresetRevisionId),
    InlineJson(String),
}

/// Request to create a durable export job. Empty item lists are rejected so a
/// job always means real work rather than a UI placeholder.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EnqueueExportJob {
    pub settings: ExportSettingsSource,
    pub items: Vec<NewExportItem>,
    pub now_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ExportJobRecord {
    pub id: ExportJobId,
    pub state: ExportJobState,
    pub preset_revision_id: Option<ExportPresetRevisionId>,
    pub settings_json: String,
    pub settings_digest: [u8; 32],
    pub created_at_ms: i64,
    pub updated_at_ms: i64,
    pub started_at_ms: Option<i64>,
    pub finished_at_ms: Option<i64>,
    pub last_error: Option<ExportFailure>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ExportItemRecord {
    pub id: ExportItemId,
    pub job_id: ExportJobId,
    pub position: u32,
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub recipe_commit_id: RecipeCommitId,
    pub recipe_snapshot_digest: [u8; 32],
    pub edit_commit_id: Option<EditCommitId>,
    pub source_identity_json: String,
    pub source_identity_digest: [u8; 32],
    pub render_plan_json: String,
    pub render_plan_digest: [u8; 32],
    pub output: AssetLocation,
    pub state: ExportItemState,
    pub attempt_count: u32,
    pub created_at_ms: i64,
    pub updated_at_ms: i64,
    pub started_at_ms: Option<i64>,
    pub finished_at_ms: Option<i64>,
    pub error: Option<ExportFailure>,
}

/// Aggregate worker state for one durable export job.
///
/// This projection deliberately counts rows in SQLite instead of materializing
/// every item. A task center can therefore refresh progress for a very large
/// job without turning the Catalog actor into an item-list transport.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq)]
pub struct ExportJobProgress {
    pub queued_item_count: u64,
    pub preparing_item_count: u64,
    pub rendering_item_count: u64,
    pub encoding_item_count: u64,
    pub writing_temp_item_count: u64,
    pub paused_conflict_item_count: u64,
    pub cancelled_item_count: u64,
    pub failed_item_count: u64,
    pub completed_item_count: u64,
    pub interrupted_item_count: u64,
    pub total_item_count: u64,
}

impl ExportJobProgress {
    /// Items currently held by a worker. This excludes queued retry work.
    pub const fn active_item_count(self) -> u64 {
        self.preparing_item_count
            + self.rendering_item_count
            + self.encoding_item_count
            + self.writing_temp_item_count
    }

    /// Items a startup worker can attempt without first resolving a conflict.
    pub const fn resumable_item_count(self) -> u64 {
        self.queued_item_count + self.interrupted_item_count
    }

    fn record(&mut self, state: ExportItemState, count: u64) -> Result<(), CatalogError> {
        let bucket = match state {
            ExportItemState::Queued => &mut self.queued_item_count,
            ExportItemState::Preparing => &mut self.preparing_item_count,
            ExportItemState::Rendering => &mut self.rendering_item_count,
            ExportItemState::Encoding => &mut self.encoding_item_count,
            ExportItemState::WritingTemp => &mut self.writing_temp_item_count,
            ExportItemState::PausedConflict => &mut self.paused_conflict_item_count,
            ExportItemState::Cancelled => &mut self.cancelled_item_count,
            ExportItemState::Failed => &mut self.failed_item_count,
            ExportItemState::Completed => &mut self.completed_item_count,
            ExportItemState::Interrupted => &mut self.interrupted_item_count,
        };
        *bucket = bucket.checked_add(count).ok_or_else(|| {
            CatalogError::InvalidExport("export job state count overflowed u64".into())
        })?;
        self.total_item_count = self.total_item_count.checked_add(count).ok_or_else(|| {
            CatalogError::InvalidExport("export item count overflowed u64".into())
        })?;
        Ok(())
    }
}

/// Receipt for one startup recovery pass.
///
/// `interrupted_item_count` records work that was actively held when the
/// process stopped. `requeued_item_count` also includes older persisted
/// `Interrupted` rows, because they are safe retry candidates too. The queue
/// count is global and intentionally has no task-center page bound.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq)]
pub struct ExportQueueRecovery {
    pub interrupted_item_count: u64,
    pub requeued_item_count: u64,
    pub queued_item_count: u64,
}

/// A compare-and-swap item transition performed by the export worker.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct AdvanceExportItem {
    pub item_id: ExportItemId,
    pub expected_state: ExportItemState,
    pub next_state: ExportItemState,
    pub failure: Option<ExportFailure>,
    pub receipt: Option<NewExportOutputReceipt>,
    pub now_ms: i64,
}

/// A conservative list bound for a task-center refresh.
pub const MAX_EXPORT_JOB_PAGE_SIZE: usize = 256;

impl Catalog {
    /// Creates a named preset and its first immutable settings revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid names/settings or a duplicate name.
    pub fn create_export_preset(
        &mut self,
        name: &str,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        validate_preset_name(name)?;
        let settings_json = normalized_json_object(settings_json, "preset settings")?;
        let preset_id = ExportPresetId::new_v7();
        let revision_id = ExportPresetRevisionId::new_v7();
        let settings_digest = json_digest(&settings_json);
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "INSERT INTO export_presets(id, name, created_at_ms) VALUES (?1, ?2, ?3)",
            params![preset_id.as_bytes().as_slice(), name.trim(), now_ms],
        )?;
        insert_preset_revision(
            &transaction,
            revision_id,
            preset_id,
            1,
            &settings_json,
            settings_digest,
            now_ms,
        )?;
        transaction.commit()?;
        Ok(ExportPresetRevisionRecord {
            id: revision_id,
            preset_id,
            revision: 1,
            settings_json,
            settings_digest,
            created_at_ms: now_ms,
        })
    }

    /// Appends one immutable revision to a named export preset.
    ///
    /// Existing jobs remain bound to their copied settings snapshot and never
    /// observe this later revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the preset is absent, settings are invalid,
    /// revision space is exhausted, or persistence fails.
    pub fn revise_export_preset(
        &mut self,
        preset_id: ExportPresetId,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        let settings_json = normalized_json_object(settings_json, "preset settings")?;
        let settings_digest = json_digest(&settings_json);
        let revision_id = ExportPresetRevisionId::new_v7();
        let transaction = self.connection.transaction()?;
        ensure_preset_exists(&transaction, preset_id)?;
        let next_revision = transaction.query_row(
            "SELECT COALESCE(MAX(revision), 0) + 1 FROM export_preset_revisions WHERE preset_id = ?1",
            [preset_id.as_bytes().as_slice()],
            |row| row.get::<_, i64>(0),
        )?;
        let revision = u32::try_from(next_revision).map_err(|_| {
            CatalogError::InvalidExport("export preset revision space is exhausted".into())
        })?;
        insert_preset_revision(
            &transaction,
            revision_id,
            preset_id,
            revision,
            &settings_json,
            settings_digest,
            now_ms,
        )?;
        transaction.commit()?;
        Ok(ExportPresetRevisionRecord {
            id: revision_id,
            preset_id,
            revision,
            settings_json,
            settings_digest,
            created_at_ms: now_ms,
        })
    }

    /// Lists named export presets without materializing their settings history.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when preset rows cannot be read.
    pub fn export_presets(&self) -> Result<Vec<ExportPresetRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, name, created_at_ms, archived_at_ms
             FROM export_presets ORDER BY name COLLATE NOCASE, id",
        )?;
        statement
            .query_map([], read_export_preset)?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Lists every immutable revision of one preset, newest first.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the preset is absent or a stored revision is invalid.
    pub fn export_preset_revisions(
        &self,
        preset_id: ExportPresetId,
    ) -> Result<Vec<ExportPresetRevisionRecord>, CatalogError> {
        ensure_preset_exists_connection(&self.connection, preset_id)?;
        let mut statement = self.connection.prepare(
            "SELECT id, preset_id, revision, settings_json, settings_digest, created_at_ms
             FROM export_preset_revisions WHERE preset_id = ?1
             ORDER BY revision DESC, id DESC",
        )?;
        statement
            .query_map(
                [preset_id.as_bytes().as_slice()],
                read_export_preset_revision,
            )?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

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
    /// Returns [`CatalogError`] when a stored item row is malformed or SQLite
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
    /// invalid, or if SQLite cannot aggregate the job rows.
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

fn insert_preset_revision(
    transaction: &Transaction<'_>,
    id: ExportPresetRevisionId,
    preset_id: ExportPresetId,
    revision: u32,
    settings_json: &str,
    settings_digest: [u8; 32],
    created_at_ms: i64,
) -> Result<(), CatalogError> {
    transaction.execute(
        "INSERT INTO export_preset_revisions(
             id, preset_id, revision, settings_json, settings_digest, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            id.as_bytes().as_slice(),
            preset_id.as_bytes().as_slice(),
            i64::from(revision),
            settings_json,
            settings_digest.as_slice(),
            created_at_ms,
        ],
    )?;
    Ok(())
}

fn resolve_settings_snapshot(
    transaction: &Transaction<'_>,
    source: &ExportSettingsSource,
) -> Result<(Option<ExportPresetRevisionId>, String, [u8; 32]), CatalogError> {
    match source {
        ExportSettingsSource::InlineJson(settings_json) => {
            let settings_json = normalized_json_object(settings_json, "export settings")?;
            let digest = json_digest(&settings_json);
            Ok((None, settings_json, digest))
        }
        ExportSettingsSource::PresetRevision(id) => {
            let snapshot = transaction
                .query_row(
                    "SELECT id, settings_json, settings_digest
                     FROM export_preset_revisions WHERE id = ?1",
                    [id.as_bytes().as_slice()],
                    |row| {
                        Ok((
                            read_id::<ExportPresetRevisionId>(row, 0)?,
                            row.get::<_, String>(1)?,
                            digest(row.get(2)?, 2)?,
                        ))
                    },
                )
                .optional()?
                .ok_or(CatalogError::ExportPresetRevisionNotFound(*id))?;
            validate_stored_json_snapshot(&snapshot.1, snapshot.2, "preset settings")?;
            Ok((Some(snapshot.0), snapshot.1, snapshot.2))
        }
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

fn ensure_preset_exists(
    transaction: &Transaction<'_>,
    preset_id: ExportPresetId,
) -> Result<(), CatalogError> {
    let exists = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM export_presets WHERE id = ?1)",
        [preset_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::ExportPresetNotFound(preset_id));
    }
    Ok(())
}

fn ensure_preset_exists_connection(
    connection: &rusqlite::Connection,
    preset_id: ExportPresetId,
) -> Result<(), CatalogError> {
    let exists = connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM export_presets WHERE id = ?1)",
        [preset_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::ExportPresetNotFound(preset_id));
    }
    Ok(())
}

fn ensure_export_job_exists(
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

fn validate_preset_name(value: &str) -> Result<(), CatalogError> {
    let trimmed = value.trim();
    if trimmed.is_empty() || trimmed.len() > 256 || trimmed.contains('\0') {
        return Err(CatalogError::InvalidExport(
            "export preset names must contain 1 through 256 non-NUL bytes".into(),
        ));
    }
    Ok(())
}

fn validate_output_location(location: &AssetLocation) -> Result<(), CatalogError> {
    if location.native_path.is_empty()
        || location.display_path.trim().is_empty()
        || location.display_path.len() > 16 * 1024
        || location.display_path.contains('\0')
    {
        return Err(CatalogError::InvalidExport(
            "export output locations require nonempty native and display paths".into(),
        ));
    }
    Ok(())
}

fn validate_receipt(
    receipt: &NewExportOutputReceipt,
    expected_output: &AssetLocation,
) -> Result<(), CatalogError> {
    validate_output_location(&receipt.output)?;
    if &receipt.output != expected_output {
        return Err(CatalogError::InvalidExport(
            "an output receipt must name the immutable item output target".into(),
        ));
    }
    if !valid_short_token(&receipt.output_format, 64) {
        return Err(CatalogError::InvalidExport(
            "export output format must be printable text up to 64 bytes".into(),
        ));
    }
    let _ = normalized_json_object(&receipt.receipt_json, "output receipt")?;
    Ok(())
}

fn validate_failure(failure: &ExportFailure) -> Result<(), CatalogError> {
    if !valid_short_token(&failure.code, 128)
        || failure.message.trim().is_empty()
        || failure.message.len() > 8192
        || failure.message.contains('\0')
    {
        return Err(CatalogError::InvalidExport(
            "export failure code/message are outside their supported bounds".into(),
        ));
    }
    Ok(())
}

fn valid_short_token(value: &str, maximum: usize) -> bool {
    !value.is_empty()
        && value.len() <= maximum
        && value
            .bytes()
            .all(|byte| byte.is_ascii_graphic() || byte == b' ')
}

fn normalized_json_object(value: &str, field: &str) -> Result<String, CatalogError> {
    let parsed: serde_json::Value = serde_json::from_str(value).map_err(|error| {
        CatalogError::InvalidExport(format!("{field} must be valid JSON: {error}"))
    })?;
    if !parsed.is_object() {
        return Err(CatalogError::InvalidExport(format!(
            "{field} must be a JSON object"
        )));
    }
    serde_json::to_string(&parsed).map_err(|error| {
        CatalogError::InvalidExport(format!("{field} cannot be normalized: {error}"))
    })
}

fn validate_stored_json_snapshot(
    value: &str,
    expected_digest: [u8; 32],
    field: &str,
) -> Result<(), CatalogError> {
    let normalized = normalized_json_object(value, field)?;
    if normalized != value {
        return Err(CatalogError::InvalidExport(format!(
            "persisted {field} JSON is not canonical"
        )));
    }
    if json_digest(value) != expected_digest {
        return Err(CatalogError::InvalidExport(format!(
            "persisted {field} digest does not match its JSON"
        )));
    }
    Ok(())
}

fn json_digest(value: &str) -> [u8; 32] {
    *blake3::hash(value.as_bytes()).as_bytes()
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

fn parse_job_state(value: &str) -> rusqlite::Result<ExportJobState> {
    match value {
        "queued" => Ok(ExportJobState::Queued),
        "running" => Ok(ExportJobState::Running),
        "paused_conflict" => Ok(ExportJobState::PausedConflict),
        "cancelled" => Ok(ExportJobState::Cancelled),
        "failed" => Ok(ExportJobState::Failed),
        "completed" => Ok(ExportJobState::Completed),
        "interrupted" => Ok(ExportJobState::Interrupted),
        _ => Err(invalid_export_row(
            0,
            format!("unknown export job state {value:?}"),
        )),
    }
}

fn parse_item_state(value: &str) -> rusqlite::Result<ExportItemState> {
    match value {
        "queued" => Ok(ExportItemState::Queued),
        "preparing" => Ok(ExportItemState::Preparing),
        "rendering" => Ok(ExportItemState::Rendering),
        "encoding" => Ok(ExportItemState::Encoding),
        "writing_temp" => Ok(ExportItemState::WritingTemp),
        "paused_conflict" => Ok(ExportItemState::PausedConflict),
        "cancelled" => Ok(ExportItemState::Cancelled),
        "failed" => Ok(ExportItemState::Failed),
        "completed" => Ok(ExportItemState::Completed),
        "interrupted" => Ok(ExportItemState::Interrupted),
        _ => Err(invalid_export_row(
            0,
            format!("unknown export item state {value:?}"),
        )),
    }
}

fn parse_platform(value: &str, index: usize) -> rusqlite::Result<Platform> {
    match value {
        "macos" => Ok(Platform::MacOs),
        "windows" => Ok(Platform::Windows),
        "other_unix" => Ok(Platform::OtherUnix),
        _ => Err(invalid_export_row(
            index,
            format!("unknown export output platform {value:?}"),
        )),
    }
}

fn read_export_preset(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportPresetRecord> {
    Ok(ExportPresetRecord {
        id: read_id(row, 0)?,
        name: row.get(1)?,
        created_at_ms: row.get(2)?,
        archived_at_ms: row.get(3)?,
    })
}

fn read_export_preset_revision(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<ExportPresetRevisionRecord> {
    let revision = row.get::<_, i64>(2)?;
    let settings_json: String = row.get(3)?;
    let settings_digest = digest(row.get(4)?, 4)?;
    validate_stored_json_snapshot(&settings_json, settings_digest, "preset settings")
        .map_err(|error| invalid_export_row(3, error.to_string()))?;
    Ok(ExportPresetRevisionRecord {
        id: read_id(row, 0)?,
        preset_id: read_id(row, 1)?,
        revision: u32::try_from(revision).map_err(|error| conversion_error(2, error))?,
        settings_json,
        settings_digest,
        created_at_ms: row.get(5)?,
    })
}

fn read_export_job(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportJobRecord> {
    let state_text: String = row.get(1)?;
    let settings_json: String = row.get(3)?;
    let settings_digest = digest(row.get(4)?, 4)?;
    validate_stored_json_snapshot(&settings_json, settings_digest, "export job settings")
        .map_err(|error| invalid_export_row(3, error.to_string()))?;
    Ok(ExportJobRecord {
        id: read_id(row, 0)?,
        state: parse_job_state(&state_text)?,
        preset_revision_id: read_optional_id(row, 2)?,
        settings_json,
        settings_digest,
        created_at_ms: row.get(5)?,
        updated_at_ms: row.get(6)?,
        started_at_ms: row.get(7)?,
        finished_at_ms: row.get(8)?,
        last_error: read_failure(row, 9)?,
    })
}

fn read_export_item(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportItemRecord> {
    let state_text: String = row.get(15)?;
    let output_platform: String = row.get(12)?;
    let position = row.get::<_, i64>(2)?;
    let attempts = row.get::<_, i64>(16)?;
    let source_identity_json: String = row.get(8)?;
    let source_identity_digest = digest(row.get(9)?, 9)?;
    validate_stored_json_snapshot(
        &source_identity_json,
        source_identity_digest,
        "export source identity",
    )
    .map_err(|error| invalid_export_row(8, error.to_string()))?;
    let render_plan_json: String = row.get(10)?;
    let render_plan_digest = digest(row.get(11)?, 11)?;
    validate_stored_json_snapshot(&render_plan_json, render_plan_digest, "export render plan")
        .map_err(|error| invalid_export_row(10, error.to_string()))?;
    Ok(ExportItemRecord {
        id: read_id(row, 0)?,
        job_id: read_id(row, 1)?,
        position: u32::try_from(position).map_err(|error| conversion_error(2, error))?,
        photo_id: read_id(row, 3)?,
        representation_id: read_id(row, 4)?,
        recipe_commit_id: read_id(row, 5)?,
        recipe_snapshot_digest: digest(row.get(6)?, 6)?,
        edit_commit_id: read_optional_edit_commit_id(row, 7)?,
        source_identity_json,
        source_identity_digest,
        render_plan_json,
        render_plan_digest,
        output: AssetLocation::new(
            parse_platform(&output_platform, 12)?,
            row.get(13)?,
            row.get::<_, String>(14)?,
        ),
        state: parse_item_state(&state_text)?,
        attempt_count: u32::try_from(attempts).map_err(|error| conversion_error(16, error))?,
        created_at_ms: row.get(17)?,
        updated_at_ms: row.get(18)?,
        started_at_ms: row.get(19)?,
        finished_at_ms: row.get(20)?,
        error: read_failure(row, 21)?,
    })
}

fn read_export_receipt(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportOutputReceiptRecord> {
    let platform: String = row.get(2)?;
    let content_digest = row
        .get::<_, Option<Vec<u8>>>(7)?
        .map(|value| digest(value, 7))
        .transpose()?;
    Ok(ExportOutputReceiptRecord {
        id: read_id(row, 0)?,
        item_id: read_id(row, 1)?,
        output: AssetLocation::new(
            parse_platform(&platform, 2)?,
            row.get(3)?,
            row.get::<_, String>(4)?,
        ),
        output_format: row.get(5)?,
        byte_len: non_negative_u64(row.get(6)?, 6)?,
        content_digest,
        receipt_json: row.get(8)?,
        created_at_ms: row.get(9)?,
    })
}

fn read_optional_id<T: EntityId>(
    row: &rusqlite::Row<'_>,
    index: usize,
) -> rusqlite::Result<Option<T>> {
    let value = row.get::<_, Option<Vec<u8>>>(index)?;
    value
        .map(|value| {
            let uuid = Uuid::from_slice(&value).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
            })?;
            Ok(T::from_uuid(uuid))
        })
        .transpose()
}

fn read_optional_edit_commit_id(
    row: &rusqlite::Row<'_>,
    index: usize,
) -> rusqlite::Result<Option<EditCommitId>> {
    row.get::<_, Option<Vec<u8>>>(index)?
        .map(|value| {
            let bytes: [u8; 32] = value.try_into().map_err(|value: Vec<u8>| {
                invalid_export_row(
                    index,
                    format!("expected 32-byte edit commit id, got {}", value.len()),
                )
            })?;
            Ok(EditCommitId::from_bytes(bytes))
        })
        .transpose()
}

fn read_failure(
    row: &rusqlite::Row<'_>,
    start_index: usize,
) -> rusqlite::Result<Option<ExportFailure>> {
    let code: Option<String> = row.get(start_index)?;
    let message: Option<String> = row.get(start_index + 1)?;
    let retryable: Option<i64> = row.get(start_index + 2)?;
    match (code, message, retryable) {
        (None, None, None) => Ok(None),
        (Some(code), Some(message), Some(retryable)) => {
            let retryable = match retryable {
                0 => false,
                1 => true,
                _ => {
                    return Err(invalid_export_row(
                        start_index + 2,
                        "invalid export retryable value",
                    ));
                }
            };
            let failure = ExportFailure {
                code,
                message,
                retryable,
            };
            validate_failure(&failure)
                .map_err(|error| invalid_export_row(start_index, error.to_string()))?;
            Ok(Some(failure))
        }
        _ => Err(invalid_export_row(
            start_index,
            "persisted export failure fields are incomplete",
        )),
    }
}

fn invalid_export_row(index: usize, message: impl Into<String>) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(
        index,
        Type::Text,
        std::io::Error::new(std::io::ErrorKind::InvalidData, message.into()).into(),
    )
}

fn conversion_error(
    index: usize,
    error: impl std::error::Error + Send + Sync + 'static,
) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
}

#[cfg(test)]
mod tests {
    use shadow_domain::{
        AssetLocation, EntityId, Platform, RecipeCommitId, RecipeId, RepresentationKind,
    };

    use super::*;
    use crate::RegisterAsset;

    const SNAPSHOT_DIGEST: [u8; 32] = [7; 32];

    fn output(path: &str) -> AssetLocation {
        AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path)
    }

    fn seeded_item(catalog: &mut Catalog) -> NewExportItem {
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/export.NEF".to_vec(),
                    "/photos/export.NEF",
                ),
                byte_len: 42,
                modified_at_ms: Some(1),
                now_ms: 1,
            })
            .expect("register source");
        let commit_id = RecipeCommitId::new_v7();
        let recipe_id = RecipeId::new_v7();
        catalog
            .connection
            .execute(
                "INSERT INTO recipe_commits(
                     id, photo_id, recipe_id, commit_json, snapshot_digest, created_at_ms
                 ) VALUES (?1, ?2, ?3, '{}', ?4, 2)",
                params![
                    commit_id.as_bytes().as_slice(),
                    registered.photo_id.as_bytes().as_slice(),
                    recipe_id.as_bytes().as_slice(),
                    SNAPSHOT_DIGEST.as_slice(),
                ],
            )
            .expect("seed immutable recipe snapshot");
        NewExportItem {
            photo_id: registered.photo_id,
            representation_id: registered.representation_id,
            recipe_commit_id: commit_id,
            recipe_snapshot_digest: SNAPSHOT_DIGEST,
            edit_commit_id: None,
            source_identity_json: r#"{"scope":"whole_file","digest":"test"}"#.into(),
            render_plan_json: r#"{"quality":"full","provider":"libraw"}"#.into(),
            output: output("/exports/export.jpg"),
        }
    }

    fn enqueue(catalog: &mut Catalog) -> ExportJobRecord {
        let item = seeded_item(catalog);
        enqueue_items(catalog, vec![item], 3)
    }

    fn enqueue_items(
        catalog: &mut Catalog,
        items: Vec<NewExportItem>,
        now_ms: i64,
    ) -> ExportJobRecord {
        catalog
            .enqueue_export_job(&EnqueueExportJob {
                settings: ExportSettingsSource::InlineJson(
                    r#"{"format":"jpeg","quality":90}"#.into(),
                ),
                items,
                now_ms,
            })
            .expect("enqueue export")
    }

    #[test]
    fn preset_revisions_are_immutable_snapshots() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let first = catalog
            .create_export_preset("Web JPEG", r#"{"format":"jpeg","quality":80}"#, 1)
            .expect("create preset");
        let second = catalog
            .revise_export_preset(first.preset_id, r#"{"format":"jpeg","quality":92}"#, 2)
            .expect("revise preset");

        assert_eq!(first.revision, 1);
        assert_eq!(second.revision, 2);
        assert_ne!(first.settings_digest, second.settings_digest);
        let revisions = catalog
            .export_preset_revisions(first.preset_id)
            .expect("list revisions");
        assert_eq!(revisions, vec![second, first]);
    }

    #[test]
    fn queued_item_freezes_recipe_and_requires_receipt_before_completion() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let job = enqueue(&mut catalog);
        let claimed = catalog
            .claim_next_export_item(4)
            .expect("claim")
            .expect("one item");
        assert_eq!(claimed.state, ExportItemState::Preparing);
        assert_eq!(claimed.attempt_count, 1);
        assert_eq!(job.state, ExportJobState::Queued);

        let err = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: claimed.id,
                expected_state: ExportItemState::Preparing,
                next_state: ExportItemState::Completed,
                failure: None,
                receipt: None,
                now_ms: 5,
            })
            .expect_err("cannot skip rendering or completion receipt");
        assert!(matches!(err, CatalogError::InvalidExportTransition { .. }));

        let rendering = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: claimed.id,
                expected_state: ExportItemState::Preparing,
                next_state: ExportItemState::Rendering,
                failure: None,
                receipt: None,
                now_ms: 5,
            })
            .expect("rendering");
        let encoding = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: rendering.id,
                expected_state: ExportItemState::Rendering,
                next_state: ExportItemState::Encoding,
                failure: None,
                receipt: None,
                now_ms: 6,
            })
            .expect("encoding");
        let writing = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: encoding.id,
                expected_state: ExportItemState::Encoding,
                next_state: ExportItemState::WritingTemp,
                failure: None,
                receipt: None,
                now_ms: 7,
            })
            .expect("writing");
        let completed = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: writing.id,
                expected_state: ExportItemState::WritingTemp,
                next_state: ExportItemState::Completed,
                failure: None,
                receipt: Some(NewExportOutputReceipt {
                    output: writing.output.clone(),
                    output_format: "jpeg".into(),
                    byte_len: 123,
                    content_digest: Some([8; 32]),
                    receipt_json: r#"{"color_space":"srgb"}"#.into(),
                }),
                now_ms: 8,
            })
            .expect("complete item");
        assert_eq!(completed.state, ExportItemState::Completed);
        assert!(
            catalog
                .export_output_receipt(completed.id)
                .expect("receipt query")
                .is_some()
        );
        assert_eq!(
            catalog.export_job(job.id).expect("job").expect("job").state,
            ExportJobState::Completed
        );
    }

    #[test]
    fn failed_and_interrupted_items_can_be_retried_without_losing_snapshot() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let job = enqueue(&mut catalog);
        let claimed = catalog
            .claim_next_export_item(4)
            .expect("claim")
            .expect("item");
        let failed = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: claimed.id,
                expected_state: ExportItemState::Preparing,
                next_state: ExportItemState::Failed,
                failure: Some(ExportFailure {
                    code: "encoder_failed".into(),
                    message: "jpeg encoder rejected the temporary file".into(),
                    retryable: true,
                }),
                receipt: None,
                now_ms: 5,
            })
            .expect("record failure");
        assert_eq!(failed.state, ExportItemState::Failed);
        assert_eq!(
            catalog.export_job(job.id).expect("job").expect("job").state,
            ExportJobState::Failed
        );

        let requeued = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: failed.id,
                expected_state: ExportItemState::Failed,
                next_state: ExportItemState::Queued,
                failure: None,
                receipt: None,
                now_ms: 6,
            })
            .expect("retry queue");
        assert_eq!(requeued.recipe_snapshot_digest, SNAPSHOT_DIGEST);
        assert!(requeued.error.is_none());
        assert!(requeued.started_at_ms.is_none());

        let preparing = catalog
            .claim_next_export_item(7)
            .expect("reclaim")
            .expect("item");
        assert_eq!(preparing.attempt_count, 2);
        let recovery = catalog
            .recover_and_requeue_interrupted_export_items(8)
            .expect("recover");
        assert_eq!(
            recovery,
            ExportQueueRecovery {
                interrupted_item_count: 1,
                requeued_item_count: 1,
                queued_item_count: 1,
            }
        );
        let queued = catalog
            .export_job_items(job.id)
            .expect("items")
            .pop()
            .expect("item");
        assert_eq!(queued.state, ExportItemState::Queued);
        assert_eq!(queued.recipe_commit_id, preparing.recipe_commit_id);
        assert_eq!(
            queued.recipe_snapshot_digest,
            preparing.recipe_snapshot_digest
        );
        assert_eq!(
            queued.source_identity_digest,
            preparing.source_identity_digest
        );
        assert_eq!(queued.render_plan_digest, preparing.render_plan_digest);
        assert_eq!(queued.output, preparing.output);
        assert_eq!(queued.attempt_count, preparing.attempt_count);
        assert!(queued.started_at_ms.is_none());
        assert!(queued.finished_at_ms.is_none());
        assert!(queued.error.is_none());
        assert_eq!(
            catalog.export_job(job.id).expect("job").expect("job").state,
            ExportJobState::Queued
        );
        assert!(
            catalog
                .export_job(job.id)
                .expect("job")
                .expect("job")
                .started_at_ms
                .is_none()
        );
    }

    #[test]
    fn indexed_item_lookup_and_progress_do_not_materialize_the_parent_job() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let first = seeded_item(&mut catalog);
        let second = NewExportItem {
            output: output("/exports/second.jpg"),
            ..first.clone()
        };
        let job = enqueue_items(&mut catalog, vec![first, second], 3);
        let claimed = catalog
            .claim_next_export_item(4)
            .expect("claim")
            .expect("one item");

        assert_eq!(
            catalog.export_item(claimed.id).expect("indexed item"),
            Some(claimed.clone())
        );
        assert!(
            catalog
                .export_item(ExportItemId::new_v7())
                .expect("missing lookup")
                .is_none()
        );
        let progress = catalog.export_job_progress(job.id).expect("progress");
        assert_eq!(progress.total_item_count, 2);
        assert_eq!(progress.queued_item_count, 1);
        assert_eq!(progress.preparing_item_count, 1);
        assert_eq!(progress.active_item_count(), 1);
        assert_eq!(progress.resumable_item_count(), 1);
    }

    #[test]
    fn startup_recovery_requeues_legacy_interrupted_rows_without_rewriting_snapshots() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let job = enqueue(&mut catalog);
        let claimed = catalog
            .claim_next_export_item(4)
            .expect("claim")
            .expect("one item");
        let interrupted = catalog
            .advance_export_item(&AdvanceExportItem {
                item_id: claimed.id,
                expected_state: ExportItemState::Preparing,
                next_state: ExportItemState::Interrupted,
                failure: Some(ExportFailure {
                    code: "worker_interrupted".into(),
                    message: "previous process stopped before atomic output commit".into(),
                    retryable: true,
                }),
                receipt: None,
                now_ms: 5,
            })
            .expect("seed legacy interrupted row");
        assert_eq!(interrupted.state, ExportItemState::Interrupted);

        let recovery = catalog
            .recover_and_requeue_interrupted_export_items(6)
            .expect("recover interrupted row");
        assert_eq!(recovery.interrupted_item_count, 0);
        assert_eq!(recovery.requeued_item_count, 1);
        assert_eq!(recovery.queued_item_count, 1);
        let queued = catalog
            .export_item(interrupted.id)
            .expect("indexed item")
            .expect("queued item");
        assert_eq!(queued.state, ExportItemState::Queued);
        assert_eq!(queued.recipe_commit_id, interrupted.recipe_commit_id);
        assert_eq!(
            queued.recipe_snapshot_digest,
            interrupted.recipe_snapshot_digest
        );
        assert_eq!(
            queued.source_identity_digest,
            interrupted.source_identity_digest
        );
        assert_eq!(queued.render_plan_digest, interrupted.render_plan_digest);
        assert_eq!(queued.output, interrupted.output);
        assert_eq!(queued.attempt_count, interrupted.attempt_count);
        assert!(queued.started_at_ms.is_none());
        assert!(queued.finished_at_ms.is_none());
        assert!(queued.error.is_none());
        assert_eq!(
            catalog.export_job(job.id).expect("job").expect("job").state,
            ExportJobState::Queued
        );
    }

    #[test]
    fn startup_recovery_is_not_limited_by_task_center_page_size() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let item = seeded_item(&mut catalog);
        let mut job_ids = Vec::with_capacity(MAX_EXPORT_JOB_PAGE_SIZE + 1);
        for index in 0..=MAX_EXPORT_JOB_PAGE_SIZE {
            let item = NewExportItem {
                output: output(&format!("/exports/recovery-{index}.jpg")),
                ..item.clone()
            };
            let now_ms = i64::try_from(index).expect("test timestamp") + 10;
            let job = enqueue_items(&mut catalog, vec![item], now_ms);
            let claimed = catalog
                .claim_next_export_item(now_ms + 1)
                .expect("claim queued item")
                .expect("one queued item");
            assert_eq!(claimed.state, ExportItemState::Preparing);
            job_ids.push(job.id);
        }

        let recovery = catalog
            .recover_and_requeue_interrupted_export_items(1_000)
            .expect("recover every active job");
        let expected = u64::try_from(MAX_EXPORT_JOB_PAGE_SIZE + 1).expect("test count");
        assert_eq!(recovery.interrupted_item_count, expected);
        assert_eq!(recovery.requeued_item_count, expected);
        assert_eq!(recovery.queued_item_count, expected);
        for index in [0, MAX_EXPORT_JOB_PAGE_SIZE / 2, MAX_EXPORT_JOB_PAGE_SIZE] {
            let job_id = job_ids[index];
            assert_eq!(
                catalog
                    .export_job(job_id)
                    .expect("job")
                    .expect("persisted job")
                    .state,
                ExportJobState::Queued
            );
            let progress = catalog.export_job_progress(job_id).expect("progress");
            assert_eq!(progress.queued_item_count, 1);
            assert_eq!(progress.total_item_count, 1);
        }
    }

    #[test]
    fn enqueue_rejects_a_recipe_digest_that_does_not_match_the_immutable_commit() {
        let mut catalog = Catalog::open_in_memory().expect("catalog");
        let mut item = seeded_item(&mut catalog);
        item.recipe_snapshot_digest = [9; 32];
        let error = catalog
            .enqueue_export_job(&EnqueueExportJob {
                settings: ExportSettingsSource::InlineJson(r#"{"format":"jpeg"}"#.into()),
                items: vec![item],
                now_ms: 3,
            })
            .expect_err("mismatched snapshot must fail");
        assert!(matches!(
            error,
            CatalogError::ExportRecipeSnapshotMismatch { .. }
        ));
    }
}
