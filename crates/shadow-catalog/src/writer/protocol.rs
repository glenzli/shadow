//! Internal request protocol between cloned Catalog handles and the single
//! connection-owning actor thread.
//!
//! Variants are grouped by feature vocabulary, while dispatch and client
//! methods remain separate consumers of this contract.

use super::*;

mod edit_history;
mod evidence;
pub(super) use edit_history::EditHistoryMessage;
pub(super) use evidence::{DecisionMessage, FeedbackMessage};

pub(super) enum Message {
    SchemaVersion(SyncSender<Result<i64, CatalogError>>),
    Stats(SyncSender<Result<CatalogStats, CatalogError>>),
    RegisterAsset(
        RegisterAsset,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RegisterAssetWithContentIdentity(
        RegisterAsset,
        ContentIdentity,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RecordRepresentationContentIdentity(
        Box<RecordRepresentationContentIdentity>,
        SyncSender<Result<RecordRepresentationContentIdentityStatus, CatalogError>>,
    ),
    RelinkMatch(
        ContentIdentity,
        SyncSender<Result<Option<RelinkMatch>, CatalogError>>,
    ),
    MissingSourceRelinkTarget(
        ImportSessionId,
        shadow_domain::LocationId,
        SyncSender<Result<Option<MissingSourceRelinkTarget>, CatalogError>>,
    ),
    UpsertPhotoLibraryFacts(Box<LibraryPhotoFacts>, SyncSender<Result<(), CatalogError>>),
    PhotoLibraryFacts(
        PhotoId,
        SyncSender<Result<Option<LibraryPhotoFacts>, CatalogError>>,
    ),
    SetPhotoLibraryState(
        Box<SetPhotoLibraryState>,
        SyncSender<Result<(), CatalogError>>,
    ),
    PhotoLibraryState(PhotoId, SyncSender<Result<PhotoLibraryState, CatalogError>>),
    CreateLibraryAlbum(
        AlbumKind,
        String,
        Option<String>,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    CreateSmartLibraryAlbum(
        String,
        SmartAlbumQueryV1,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    RenameLibraryAlbum(
        CollectionId,
        String,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    ReplaceSmartAlbumQuery(
        CollectionId,
        SmartAlbumQueryV1,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    DeleteLibraryAlbum(CollectionId, SyncSender<Result<bool, CatalogError>>),
    LibraryAlbums(SyncSender<Result<Vec<AlbumRecord>, CatalogError>>),
    AddPhotoToAlbum(
        CollectionId,
        PhotoId,
        i64,
        i64,
        SyncSender<Result<(), CatalogError>>,
    ),
    RemovePhotoFromAlbum(
        CollectionId,
        PhotoId,
        SyncSender<Result<bool, CatalogError>>,
    ),
    AlbumsForPhoto(PhotoId, SyncSender<Result<Vec<AlbumRecord>, CatalogError>>),
    LibrarySources(SyncSender<Result<Vec<LibrarySourceRecord>, CatalogError>>),
    LibrarySourceHealth(SyncSender<Result<Vec<LibrarySourceHealth>, CatalogError>>),
    MissingSourceLocationPage(
        ImportSessionId,
        Option<MissingSourceLocationCursor>,
        usize,
        SyncSender<Result<Option<MissingSourceLocationPage>, CatalogError>>,
    ),
    LibraryPhotoPage(
        LibraryPhotoFilter,
        Option<LibraryPhotoCursor>,
        usize,
        SyncSender<Result<LibraryPhotoPage, CatalogError>>,
    ),
    LibraryFacetPage(
        LibraryPhotoFilter,
        LibraryFacetKind,
        Option<LibraryFacetCursor>,
        usize,
        SyncSender<Result<LibraryFacetPage, CatalogError>>,
    ),
    LibraryPhotoCount(LibraryPhotoFilter, SyncSender<Result<u64, CatalogError>>),
    SmartAlbumFilter(
        CollectionId,
        SyncSender<Result<LibraryPhotoFilter, CatalogError>>,
    ),
    SmartAlbumPhotoPage(
        CollectionId,
        Option<LibraryPhotoCursor>,
        usize,
        SyncSender<Result<LibraryPhotoPage, CatalogError>>,
    ),
    SmartAlbumPhotoCount(CollectionId, SyncSender<Result<u64, CatalogError>>),
    RepresentationFingerprint(
        RepresentationId,
        SyncSender<Result<RepresentationFingerprint, CatalogError>>,
    ),
    RecordDecodeSnapshot(
        Box<RecordDecodeSnapshot>,
        SyncSender<Result<RecordDecodeSnapshotStatus, CatalogError>>,
    ),
    DecodeSnapshots(
        RepresentationId,
        SyncSender<Result<Vec<DecodeSnapshotRecord>, CatalogError>>,
    ),
    IsDecodeOutputCurrent(
        RepresentationId,
        String,
        String,
        RepresentationFingerprint,
        bool,
        String,
        Option<String>,
        SyncSender<Result<bool, CatalogError>>,
    ),
    RecordCachedArtifact(
        Box<RecordCachedArtifact>,
        SyncSender<Result<RecordCachedArtifactStatus, CatalogError>>,
    ),
    CachedArtifacts(
        RepresentationId,
        SyncSender<Result<Vec<CachedArtifactRecord>, CatalogError>>,
    ),
    PreferredCachedArtifact(
        RepresentationId,
        SyncSender<Result<Option<CachedArtifactRecord>, CatalogError>>,
    ),
    PreferredCachedArtifacts(
        Vec<RepresentationId>,
        SyncSender<Result<Vec<Option<CachedArtifactRecord>>, CatalogError>>,
    ),
    LiveCachedArtifactBlobs(SyncSender<Result<Vec<LiveCachedArtifactBlob>, CatalogError>>),
    InvalidateCachedArtifact(
        Box<CachedArtifactRecord>,
        SyncSender<Result<InvalidateCachedArtifactStatus, CatalogError>>,
    ),
    RecordTechnicalObservation(
        Box<RecordTechnicalObservation>,
        SyncSender<Result<RecordTechnicalObservationStatus, CatalogError>>,
    ),
    TechnicalObservation(
        RepresentationId,
        RepresentationFingerprint,
        Box<crate::CachedArtifact>,
        TechnicalObservationRevision,
        SyncSender<Result<Option<TechnicalObservationRecord>, CatalogError>>,
    ),
    ReviewPage(
        Option<ReviewCursor>,
        usize,
        Option<TechnicalObservationRevision>,
        Option<CachedArtifactGeneratorIdentity>,
        SyncSender<Result<ReviewPageRecord, CatalogError>>,
    ),
    ReviewSource(
        PhotoId,
        Option<TechnicalObservationRevision>,
        SyncSender<Result<Option<ReviewItemRecord>, CatalogError>>,
    ),
    PhotoSource(
        PhotoId,
        SyncSender<Result<Option<ReviewItemRecord>, CatalogError>>,
    ),
    EditHistory(EditHistoryMessage),
    CreateExportPreset(
        String,
        String,
        i64,
        SyncSender<Result<ExportPresetRevisionRecord, CatalogError>>,
    ),
    ReviseExportPreset(
        ExportPresetId,
        String,
        i64,
        SyncSender<Result<ExportPresetRevisionRecord, CatalogError>>,
    ),
    ExportPresets(SyncSender<Result<Vec<ExportPresetRecord>, CatalogError>>),
    ExportPresetRevisions(
        ExportPresetId,
        SyncSender<Result<Vec<ExportPresetRevisionRecord>, CatalogError>>,
    ),
    EnqueueExportJob(
        Box<EnqueueExportJob>,
        SyncSender<Result<ExportJobRecord, CatalogError>>,
    ),
    ExportJob(
        ExportJobId,
        SyncSender<Result<Option<ExportJobRecord>, CatalogError>>,
    ),
    ExportJobs(
        usize,
        SyncSender<Result<Vec<ExportJobRecord>, CatalogError>>,
    ),
    ExportJobItems(
        ExportJobId,
        SyncSender<Result<Vec<ExportItemRecord>, CatalogError>>,
    ),
    ExportItem(
        ExportItemId,
        SyncSender<Result<Option<ExportItemRecord>, CatalogError>>,
    ),
    ExportJobProgress(
        ExportJobId,
        SyncSender<Result<ExportJobProgress, CatalogError>>,
    ),
    ClaimNextExportItem(
        i64,
        SyncSender<Result<Option<ExportItemRecord>, CatalogError>>,
    ),
    AdvanceExportItem(
        Box<crate::AdvanceExportItem>,
        SyncSender<Result<ExportItemRecord, CatalogError>>,
    ),
    ExportOutputReceipt(
        ExportItemId,
        SyncSender<Result<Option<ExportOutputReceiptRecord>, CatalogError>>,
    ),
    CancelExportJob(
        ExportJobId,
        i64,
        SyncSender<Result<ExportJobRecord, CatalogError>>,
    ),
    RecoverInterruptedExportJobs(i64, SyncSender<Result<usize, CatalogError>>),
    RecoverAndRequeueInterruptedExportItems(
        i64,
        SyncSender<Result<ExportQueueRecovery, CatalogError>>,
    ),
    Decision(DecisionMessage),
    Feedback(FeedbackMessage),
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
    Shutdown(SyncSender<()>),
}
