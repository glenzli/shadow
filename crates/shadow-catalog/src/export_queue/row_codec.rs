//! Persisted export-row decoding and integrity verification.

use rusqlite::types::Type;
use shadow_domain::{AssetLocation, EditCommitId, EntityId, Platform};
use uuid::Uuid;

use crate::{
    cache_artifact::{digest, non_negative_u64},
    row_codec::read_id,
};

use super::{
    model::{
        ExportFailure, ExportItemRecord, ExportItemState, ExportJobRecord, ExportJobState,
        ExportOutputReceiptRecord, ExportPresetRecord, ExportPresetRevisionRecord,
    },
    validation::{validate_failure, validate_stored_json_snapshot},
};

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

pub(super) fn parse_item_state(value: &str) -> rusqlite::Result<ExportItemState> {
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

pub(super) fn read_export_preset(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportPresetRecord> {
    Ok(ExportPresetRecord {
        id: read_id(row, 0)?,
        name: row.get(1)?,
        created_at_ms: row.get(2)?,
        archived_at_ms: row.get(3)?,
    })
}

pub(super) fn read_export_preset_revision(
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

pub(super) fn read_export_job(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportJobRecord> {
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

pub(super) fn read_export_item(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExportItemRecord> {
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

pub(super) fn read_export_receipt(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<ExportOutputReceiptRecord> {
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

pub(super) fn read_failure(
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
