//! Actor messages for named export presets and immutable revisions.

use std::sync::mpsc::SyncSender;

use crate::{CatalogError, ExportPresetId, ExportPresetRecord, ExportPresetRevisionRecord};

pub(in crate::writer) enum ExportPresetMessage {
    Create(
        String,
        String,
        i64,
        SyncSender<Result<ExportPresetRevisionRecord, CatalogError>>,
    ),
    Revise(
        ExportPresetId,
        String,
        i64,
        SyncSender<Result<ExportPresetRevisionRecord, CatalogError>>,
    ),
    List(SyncSender<Result<Vec<ExportPresetRecord>, CatalogError>>),
    Revisions(
        ExportPresetId,
        SyncSender<Result<Vec<ExportPresetRevisionRecord>, CatalogError>>,
    ),
}
