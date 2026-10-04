//! Versioned, bounded headless requests. These are operations on the existing
//! Recipe, not a second serialization of private editable state.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const SCHEMA_VERSION: u32 = 1;
pub const MAX_REQUEST_BYTES: usize = 65_536;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct Request {
    pub schema_version: u32,
    pub command: Command,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub(super) enum Command {
    Capabilities {},
    Init {},
    Import {
        source: PathBuf,
    },
    Photos {
        #[serde(default)]
        filter: Box<shadow_catalog::LibraryPhotoFilter>,
        #[serde(default)]
        after: Option<PhotoCursor>,
        limit: u32,
    },
    Snapshot {
        photo_id: String,
        #[serde(default)]
        commit_id: Option<String>,
    },
    Commit {
        guard: SnapshotGuard,
        edits: Vec<Edit>,
    },
    History {
        photo_id: String,
        #[serde(default)]
        after: Option<HistoryCursor>,
        limit: u32,
    },
    Export {
        guard: SnapshotGuard,
        destination: PathBuf,
        bit_depth: u8,
    },
}

/// Content ancestry is independent of the expected movable head. All fields
/// are mandatory, including an empty head/base for a never-edited photograph.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct SnapshotGuard {
    pub photo_id: String,
    pub representation_id: String,
    pub source_sha256: String,
    pub base_commit_id: String,
    pub expected_working_commit_id: String,
    pub expected_active_variant_id: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct PhotoCursor {
    pub photo_id: String,
    pub file_name: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct HistoryCursor {
    pub created_at_ms: i64,
    pub commit_id: String,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub(super) enum Edit {
    SetBasic {
        node_index: usize,
        values: BasicPatch,
    },
    SetNode {
        node_index: usize,
        #[serde(default)]
        enabled: Option<bool>,
        #[serde(default)]
        opacity: Option<f64>,
        #[serde(default)]
        label: Option<String>,
    },
    AddNode {
        label: String,
    },
    RemoveNode {
        node_index: usize,
    },
    MoveNode {
        from: usize,
        to: usize,
    },
    SetGeometry {
        values: GeometryPatch,
    },
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct BasicPatch {
    pub exposure_stops: Option<f64>,
    pub contrast_factor: Option<f64>,
    pub white_balance_temperature: Option<f64>,
    pub white_balance_tint: Option<f64>,
    pub saturation_factor: Option<f64>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct GeometryPatch {
    pub enabled: Option<bool>,
    pub crop_left: Option<f64>,
    pub crop_top: Option<f64>,
    pub crop_right: Option<f64>,
    pub crop_bottom: Option<f64>,
    pub quarter_turn: Option<u8>,
    pub straighten_degrees: Option<f64>,
    pub perspective_vertical: Option<f64>,
    pub perspective_horizontal: Option<f64>,
    pub flip_horizontal: Option<bool>,
    pub flip_vertical: Option<bool>,
}

/// Exactly one JSON response per request. A failed response never represents
/// a successful commit; successful receipts come directly from publication.
#[derive(Debug, Serialize)]
pub struct Response {
    pub schema_version: u32,
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<ErrorResponse>,
}

#[derive(Debug, Serialize)]
pub struct ErrorResponse {
    pub code: String,
    pub message: String,
}

impl Response {
    pub fn failure(code: &str, message: impl Into<String>) -> Self {
        Self {
            schema_version: SCHEMA_VERSION,
            ok: false,
            result: None,
            error: Some(ErrorResponse {
                code: code.into(),
                message: message.into(),
            }),
        }
    }

    pub(super) fn success(result: Value) -> Self {
        Self {
            schema_version: SCHEMA_VERSION,
            ok: true,
            result: Some(result),
            error: None,
        }
    }
}
