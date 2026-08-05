//! Actor messages for durable import journals and explicit source relocation.

use std::sync::mpsc::SyncSender;

use shadow_domain::{AssetLocation, ImportSessionId, RepresentationId};

use crate::{
    CatalogError, ContentIdentity, ImportPhotoGrouping, ImportSession, ImportSessionState,
    ImportSessionSummary, RegisterAsset, RegisteredAsset,
};

pub(in crate::writer) enum ImportJournalMessage {
    BeginImportSession(
        AssetLocation,
        i64,
        SyncSender<Result<ImportSessionId, CatalogError>>,
    ),
    BeginRelocationSession(
        AssetLocation,
        i64,
        SyncSender<Result<ImportSessionId, CatalogError>>,
    ),
    ResumeImportSession(
        ImportSessionId,
        i64,
        SyncSender<Result<ImportSession, CatalogError>>,
    ),
    RecordImportDiscovered(
        ImportSessionId,
        RegisterAsset,
        SyncSender<Result<(), CatalogError>>,
    ),
    RegisterImportAsset(
        ImportSessionId,
        RegisterAsset,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RegisterImportAssetGrouped(
        ImportSessionId,
        RegisterAsset,
        ImportPhotoGrouping,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RegisterImportVerifiedRelocation(
        ImportSessionId,
        RegisterAsset,
        RepresentationId,
        ContentIdentity,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RecordImportIssue(
        ImportSessionId,
        AssetLocation,
        String,
        i64,
        SyncSender<Result<(), CatalogError>>,
    ),
    FinishImportSession(
        ImportSessionId,
        ImportSessionState,
        Option<String>,
        i64,
        SyncSender<Result<(), CatalogError>>,
    ),
    ImportSessionSummary(
        ImportSessionId,
        SyncSender<Result<ImportSessionSummary, CatalogError>>,
    ),
    UnfinishedImportSessions(SyncSender<Result<Vec<ImportSession>, CatalogError>>),
}
