//! Public export identities, immutable snapshots, states, and progress values.

use std::fmt;

use serde::{Deserialize, Serialize};
use shadow_domain::{
    AssetLocation, EditCommitId, EntityId, PhotoId, RecipeCommitId, RepresentationId,
};
use uuid::Uuid;

use crate::CatalogError;

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

    pub(super) const fn is_terminal(self) -> bool {
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

    pub(super) const fn is_terminal(self) -> bool {
        matches!(self, Self::Cancelled | Self::Failed | Self::Completed)
    }

    pub(super) const fn allows_transition_to(self, next: Self) -> bool {
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
/// This projection deliberately counts rows in `SQLite` instead of materializing
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

    pub(super) fn record(
        &mut self,
        state: ExportItemState,
        count: u64,
    ) -> Result<(), CatalogError> {
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
