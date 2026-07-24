use std::{
    collections::BTreeSet,
    path::Path,
    sync::mpsc::{self, Receiver, Sender, SyncSender},
    thread::{self, JoinHandle},
};

use shadow_ai::{
    FeedbackEvent, FeedbackForgetFact, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact,
};
use shadow_domain::{
    AssetLocation, CollectionId, EditCommitId, EditObjectId, ImportSessionId,
    NewPhotoDecisionEvent, PhotoDecisionEvent, PhotoDecisionState, PhotoId, RecipeCommitId,
    RepresentationId,
};

use crate::{
    AlbumKind, AlbumRecord, CachedArtifactGeneratorIdentity, CachedArtifactRecord, Catalog,
    CatalogError, CatalogStats, CatalogStore, CommitEditRepository, CommitRecipe,
    CommitRecipeAndEditRepository, CommitRecipeAndEditRepositoryResult, ContentIdentity,
    DecodeSnapshotRecord, EditObjectPackWrite, EditObjectRecord, EditRepositoryCommitRecord,
    EditRepositoryRefRecord, FeedbackPage, ImportSession, ImportSessionState, ImportSessionSummary,
    InvalidateCachedArtifactStatus, LibraryPhotoCursor, LibraryPhotoFacts, LibraryPhotoFilter,
    LibraryPhotoPage, LibrarySourceRecord, PhotoDecisionPage, PhotoLibraryState,
    RecipeCommitRecord, RecipeRefRecord, RecordCachedArtifact, RecordCachedArtifactStatus,
    RecordDecodeSnapshot, RecordDecodeSnapshotStatus, RecordTechnicalObservation,
    RecordTechnicalObservationStatus, RegisterAsset, RegisteredAsset, RepresentationFingerprint,
    ReviewCursor, ReviewItemRecord, ReviewPageRecord, SetPhotoLibraryState, SetRecipeRef,
    StoreEditObjectPackResult, TechnicalObservationRecord, TechnicalObservationRevision,
};

#[derive(Debug)]
pub struct CatalogActor {
    handle: CatalogHandle,
    join_handle: Option<JoinHandle<()>>,
}

#[derive(Debug, Clone)]
pub struct CatalogHandle {
    sender: Sender<Message>,
}

enum Message {
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
        RepresentationId,
        ContentIdentity,
        i64,
        SyncSender<Result<(), CatalogError>>,
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
    LibraryPhotoPage(
        LibraryPhotoFilter,
        Option<LibraryPhotoCursor>,
        usize,
        SyncSender<Result<LibraryPhotoPage, CatalogError>>,
    ),
    LibraryPhotoCount(LibraryPhotoFilter, SyncSender<Result<u64, CatalogError>>),
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
    CommitRecipe(
        Box<CommitRecipe>,
        SyncSender<Result<RecipeCommitRecord, CatalogError>>,
    ),
    RecipeCommits(
        PhotoId,
        SyncSender<Result<Vec<RecipeCommitRecord>, CatalogError>>,
    ),
    RecipeCommit(
        PhotoId,
        RecipeCommitId,
        SyncSender<Result<Option<RecipeCommitRecord>, CatalogError>>,
    ),
    RecipeRef(
        PhotoId,
        String,
        SyncSender<Result<Option<RecipeRefRecord>, CatalogError>>,
    ),
    SetRecipeRef(Box<SetRecipeRef>, SyncSender<Result<(), CatalogError>>),
    DiscardRecipeHistory(PhotoId, SyncSender<Result<usize, CatalogError>>),
    StoreEditObjectPack(
        Box<EditObjectPackWrite>,
        SyncSender<Result<StoreEditObjectPackResult, CatalogError>>,
    ),
    EditObject(
        EditObjectId,
        SyncSender<Result<Option<EditObjectRecord>, CatalogError>>,
    ),
    CommitEditRepository(
        Box<CommitEditRepository>,
        SyncSender<Result<EditRepositoryCommitRecord, CatalogError>>,
    ),
    CommitRecipeAndEditRepository(
        Box<CommitRecipeAndEditRepository>,
        SyncSender<Result<CommitRecipeAndEditRepositoryResult, CatalogError>>,
    ),
    EditRepositoryCommit(
        EditCommitId,
        SyncSender<Result<Option<EditRepositoryCommitRecord>, CatalogError>>,
    ),
    EditRepositoryRef(
        String,
        SyncSender<Result<Option<EditRepositoryRefRecord>, CatalogError>>,
    ),
    Decision(DecisionMessage),
    Feedback(FeedbackMessage),
    BeginImportSession(
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

enum DecisionMessage {
    State(
        PhotoId,
        SyncSender<Result<PhotoDecisionState, CatalogError>>,
    ),
    Append(
        Box<NewPhotoDecisionEvent>,
        SyncSender<Result<PhotoDecisionEvent, CatalogError>>,
    ),
    EventsAfter(
        PhotoId,
        u64,
        usize,
        SyncSender<Result<PhotoDecisionPage, CatalogError>>,
    ),
}

enum FeedbackMessage {
    AppendEvent(
        Box<NewFeedbackEvent>,
        SyncSender<Result<FeedbackEvent, CatalogError>>,
    ),
    EventsAfter(
        LearningScope,
        u64,
        usize,
        SyncSender<Result<FeedbackPage, CatalogError>>,
    ),
    AppendForgetFact(
        Box<NewFeedbackForgetFact>,
        SyncSender<Result<FeedbackForgetFact, CatalogError>>,
    ),
    ForgottenEventIds(
        LearningScope,
        SyncSender<Result<BTreeSet<String>, CatalogError>>,
    ),
}

impl CatalogActor {
    /// Starts a dedicated thread that exclusively owns the catalog connection.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the thread cannot start or the catalog cannot
    /// be opened and migrated.
    pub fn spawn(path: &Path) -> Result<Self, CatalogError> {
        let path = path.to_path_buf();
        Self::spawn_with(move || Catalog::open(&path))
    }

    #[cfg(test)]
    fn spawn_in_memory() -> Result<Self, CatalogError> {
        Self::spawn_with(Catalog::open_in_memory)
    }

    fn spawn_with(
        open_catalog: impl FnOnce() -> Result<Catalog, CatalogError> + Send + 'static,
    ) -> Result<Self, CatalogError> {
        let (sender, receiver) = mpsc::channel();
        let (ready_sender, ready_receiver) = mpsc::sync_channel(0);
        let join_handle = thread::Builder::new()
            .name("shadow-catalog-writer".to_owned())
            .spawn(move || match open_catalog() {
                Ok(catalog) => {
                    let _ = ready_sender.send(Ok(()));
                    run_actor(catalog, &receiver);
                }
                Err(error) => {
                    let _ = ready_sender.send(Err(error));
                }
            })
            .map_err(CatalogError::ActorStart)?;

        match ready_receiver.recv() {
            Ok(Ok(())) => Ok(Self {
                handle: CatalogHandle { sender },
                join_handle: Some(join_handle),
            }),
            Ok(Err(error)) => {
                let _ = join_handle.join();
                Err(error)
            }
            Err(_) => {
                let _ = join_handle.join();
                Err(CatalogError::ActorUnavailable)
            }
        }
    }

    pub fn handle(&self) -> CatalogHandle {
        self.handle.clone()
    }

    /// Stops the writer after all previously submitted commands.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor stopped unexpectedly or panicked.
    pub fn shutdown(mut self) -> Result<(), CatalogError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<(), CatalogError> {
        let Some(join_handle) = self.join_handle.take() else {
            return Ok(());
        };
        let (response_sender, response_receiver) = mpsc::sync_channel(0);
        self.handle
            .sender
            .send(Message::Shutdown(response_sender))
            .map_err(|_| CatalogError::ActorUnavailable)?;
        response_receiver
            .recv()
            .map_err(|_| CatalogError::ActorUnavailable)?;
        join_handle.join().map_err(|_| CatalogError::ActorPanicked)
    }
}

impl Drop for CatalogActor {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

impl CatalogHandle {
    /// Returns the migrated schema version.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn schema_version(&self) -> Result<i64, CatalogError> {
        self.request(Message::SchemaVersion)
    }

    /// Returns current entity counts.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn stats(&self) -> Result<CatalogStats, CatalogError> {
        self.request(Message::Stats)
    }

    /// Registers an asset outside an import journal.
    ///
    /// This is reserved for focused tools and tests. Production import should
    /// use [`CatalogStore::register_import_asset`] so journal state is atomic.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or registration
    /// fails.
    pub fn register_asset(&self, request: &RegisterAsset) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| Message::RegisterAsset(request.clone(), response))
    }

    /// Registers a location while preserving photo identity when a separately
    /// verified path-independent identity has already been recorded.
    pub fn register_asset_with_content_identity(
        &self,
        request: &RegisterAsset,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::RegisterAssetWithContentIdentity(request.clone(), identity.clone(), response)
        })
    }

    /// Stores a full-file, format-payload, or decoder-mosaic identity through
    /// the single writer. Expensive identity generation stays outside the
    /// actor; the actor only validates and persists its result.
    pub fn record_representation_content_identity(
        &self,
        representation_id: RepresentationId,
        identity: &ContentIdentity,
        observed_at_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::RecordRepresentationContentIdentity(
                representation_id,
                identity.clone(),
                observed_at_ms,
                response,
            )
        })
    }

    /// Updates the compact, indexed photo facts projection after metadata
    /// extraction. It does not persist full EXIF again.
    pub fn upsert_photo_library_facts(
        &self,
        facts: &LibraryPhotoFacts,
    ) -> Result<(), CatalogError> {
        self.request(|response| Message::UpsertPhotoLibraryFacts(Box::new(facts.clone()), response))
    }

    pub fn photo_library_facts(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<LibraryPhotoFacts>, CatalogError> {
        self.request(|response| Message::PhotoLibraryFacts(photo_id, response))
    }

    /// Sets an independent Library like/color state for a photo.
    pub fn set_photo_library_state(
        &self,
        state: &SetPhotoLibraryState,
    ) -> Result<(), CatalogError> {
        self.request(|response| Message::SetPhotoLibraryState(Box::new(state.clone()), response))
    }

    pub fn photo_library_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryState, CatalogError> {
        self.request(|response| Message::PhotoLibraryState(photo_id, response))
    }

    pub fn create_library_album(
        &self,
        kind: AlbumKind,
        name: &str,
        query_json: Option<&str>,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::CreateLibraryAlbum(
                kind,
                name.to_owned(),
                query_json.map(str::to_owned),
                now_ms,
                response,
            )
        })
    }

    pub fn library_albums(&self) -> Result<Vec<AlbumRecord>, CatalogError> {
        self.request(Message::LibraryAlbums)
    }

    pub fn add_photo_to_album(
        &self,
        album_id: CollectionId,
        photo_id: PhotoId,
        sort_key: i64,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::AddPhotoToAlbum(album_id, photo_id, sort_key, now_ms, response)
        })
    }

    pub fn remove_photo_from_album(
        &self,
        album_id: CollectionId,
        photo_id: PhotoId,
    ) -> Result<bool, CatalogError> {
        self.request(|response| Message::RemovePhotoFromAlbum(album_id, photo_id, response))
    }

    pub fn albums_for_photo(&self, photo_id: PhotoId) -> Result<Vec<AlbumRecord>, CatalogError> {
        self.request(|response| Message::AlbumsForPhoto(photo_id, response))
    }

    pub fn library_sources(&self) -> Result<Vec<LibrarySourceRecord>, CatalogError> {
        self.request(Message::LibrarySources)
    }

    /// Reads a bounded photo-first Library page through the single catalog
    /// actor. The cursor is stable across folders being renamed or reorganized.
    pub fn library_photo_page(
        &self,
        filter: &LibraryPhotoFilter,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        self.request(|response| {
            Message::LibraryPhotoPage(filter.clone(), after.cloned(), requested_limit, response)
        })
    }

    /// Counts a settled Library filter through the actor. Grid scrolling uses
    /// `library_photo_page`; this explicit aggregate can be debounced.
    pub fn library_photo_count(&self, filter: &LibraryPhotoFilter) -> Result<u64, CatalogError> {
        self.request(|response| Message::LibraryPhotoCount(filter.clone(), response))
    }

    /// Returns the current source fingerprint for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the representation
    /// is absent, or the query fails.
    pub fn representation_fingerprint(
        &self,
        representation_id: RepresentationId,
    ) -> Result<RepresentationFingerprint, CatalogError> {
        self.request(|response| Message::RepresentationFingerprint(representation_id, response))
    }

    /// Records one decoder snapshot through the single catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or persistence
    /// fails. A normal source race is reported in the returned status.
    pub fn record_decode_snapshot(
        &self,
        request: &RecordDecodeSnapshot,
    ) -> Result<RecordDecodeSnapshotStatus, CatalogError> {
        self.request(|response| Message::RecordDecodeSnapshot(Box::new(request.clone()), response))
    }

    /// Returns all current provider snapshots for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the representation
    /// is absent, or persisted state cannot be read.
    pub fn decode_snapshots(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<DecodeSnapshotRecord>, CatalogError> {
        self.request(|response| Message::DecodeSnapshots(representation_id, response))
    }

    /// Reports whether a provider's required output is current for a source.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    #[allow(clippy::too_many_arguments)]
    pub fn is_decode_output_current(
        &self,
        representation_id: RepresentationId,
        provider_id: &str,
        provider_version: &str,
        source: RepresentationFingerprint,
        require_cached_preview: bool,
        proxy_variant_key: &str,
        required_technical_preprocessing: Option<&str>,
    ) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::IsDecodeOutputCurrent(
                representation_id,
                provider_id.to_owned(),
                provider_version.to_owned(),
                source,
                require_cached_preview,
                proxy_variant_key.to_owned(),
                required_technical_preprocessing.map(str::to_owned),
                response,
            )
        })
    }

    /// Records a content-addressed cached artifact through the catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or persistence
    /// fails. Source races are returned as a normal status.
    pub fn record_cached_artifact(
        &self,
        request: &RecordCachedArtifact,
    ) -> Result<RecordCachedArtifactStatus, CatalogError> {
        self.request(|response| Message::RecordCachedArtifact(Box::new(request.clone()), response))
    }

    /// Returns cached preview/proxy references for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn cached_artifacts(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<CachedArtifactRecord>, CatalogError> {
        self.request(|response| Message::CachedArtifacts(representation_id, response))
    }

    /// Returns the shared Review-selected current visual through the Catalog
    /// writer so analysis and UI use one artifact-selection contract.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the
    /// representation is absent, or persisted metadata is invalid.
    pub fn preferred_cached_artifact(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Option<CachedArtifactRecord>, CatalogError> {
        self.request(|response| Message::PreferredCachedArtifact(representation_id, response))
    }

    /// Invalidates an exact cache reference through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the conditional
    /// delete fails.
    pub fn invalidate_cached_artifact(
        &self,
        record: &CachedArtifactRecord,
    ) -> Result<InvalidateCachedArtifactStatus, CatalogError> {
        self.request(|response| {
            Message::InvalidateCachedArtifact(Box::new(record.clone()), response)
        })
    }

    /// Records one display-luma observation through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or persistence
    /// fails. A normal source/artifact race is returned in the status.
    pub fn record_technical_observation(
        &self,
        request: &RecordTechnicalObservation,
    ) -> Result<RecordTechnicalObservationStatus, CatalogError> {
        self.request(|response| {
            Message::RecordTechnicalObservation(Box::new(request.clone()), response)
        })
    }

    /// Reads one exact, current technical observation through the Catalog actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor is unavailable or matching
    /// persisted state fails integrity validation.
    pub fn technical_observation(
        &self,
        representation_id: RepresentationId,
        expected_source: RepresentationFingerprint,
        expected_artifact: &crate::CachedArtifact,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<TechnicalObservationRecord>, CatalogError> {
        self.request(|response| {
            Message::TechnicalObservation(
                representation_id,
                expected_source,
                Box::new(expected_artifact.clone()),
                revision.clone(),
                response,
            )
        })
    }

    /// Returns one immutable, keyset-paginated Review-grid page through the
    /// Catalog actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn review_page(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| Message::ReviewPage(after.cloned(), limit, None, None, response))
    }

    /// Returns a Review page with summaries for one exact technical revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the query fails,
    /// or a matching persisted observation fails integrity validation.
    pub fn review_page_with_technical(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
        revision: &TechnicalObservationRevision,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewPage(
                after.cloned(),
                limit,
                Some(revision.clone()),
                None,
                response,
            )
        })
    }

    /// Returns a Review page whose Recipe-preview candidates must match one
    /// exact generator implementation identity.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the query fails,
    /// or a matching persisted observation fails integrity validation.
    pub fn review_page_with_technical_and_recipe_preview_generator(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
        revision: &TechnicalObservationRevision,
        recipe_preview_generator: &CachedArtifactGeneratorIdentity,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewPage(
                after.cloned(),
                limit,
                Some(revision.clone()),
                Some(recipe_preview_generator.clone()),
                response,
            )
        })
    }

    /// Returns the catalog-owned online RAW source for one photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn review_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| Message::ReviewSource(photo_id, None, response))
    }

    /// Returns one Review source with an exact current technical summary.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor is unavailable, the query fails,
    /// or a matching persisted observation fails integrity validation.
    pub fn review_source_with_technical(
        &self,
        photo_id: PhotoId,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| Message::ReviewSource(photo_id, Some(revision.clone()), response))
    }

    /// Returns the online original source for one photo, preferring RAW and
    /// falling back to an original raster.
    ///
    /// Unlike [`Self::review_source`], this is for consumers with a
    /// source-neutral decoder route rather than legacy RAW-only callers.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn photo_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| Message::PhotoSource(photo_id, response))
    }

    /// Persists an immutable Recipe commit and optional ref move through the
    /// single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the commit is invalid, a guarded ref has
    /// changed since it was read, or persistence fails.
    pub fn commit_recipe(
        &self,
        request: &CommitRecipe,
    ) -> Result<RecipeCommitRecord, CatalogError> {
        self.request(|response| Message::CommitRecipe(Box::new(request.clone()), response))
    }

    /// Lists every immutable Recipe commit owned by a photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor or persisted data is invalid.
    pub fn recipe_commits(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<RecipeCommitRecord>, CatalogError> {
        self.request(|response| Message::RecipeCommits(photo_id, response))
    }

    /// Resolves one immutable Recipe commit by owner and id.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor or persisted data is invalid.
    pub fn recipe_commit(
        &self,
        photo_id: PhotoId,
        commit_id: RecipeCommitId,
    ) -> Result<Option<RecipeCommitRecord>, CatalogError> {
        self.request(|response| Message::RecipeCommit(photo_id, commit_id, response))
    }

    /// Resolves a named Recipe ref without changing it.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name or unavailable actor.
    pub fn recipe_ref(
        &self,
        photo_id: PhotoId,
        name: &str,
    ) -> Result<Option<RecipeRefRecord>, CatalogError> {
        self.request(|response| Message::RecipeRef(photo_id, name.to_owned(), response))
    }

    /// Moves a Recipe ref through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the target does not belong to the photo or
    /// the write fails.
    pub fn set_recipe_ref(&self, request: &SetRecipeRef) -> Result<(), CatalogError> {
        self.request(|response| Message::SetRecipeRef(Box::new(request.clone()), response))
    }

    /// Discards all editable Recipe history for a single photo.
    ///
    /// This is intentionally not part of normal editing. The desktop uses it
    /// only after a user confirms that an obsolete development Recipe may be
    /// reset to the current fixed contract.
    pub fn discard_recipe_history(&self, photo_id: PhotoId) -> Result<usize, CatalogError> {
        self.request(|response| Message::DiscardRecipeHistory(photo_id, response))
    }

    /// Stores a topologically unordered pack of immutable edit objects through
    /// the Catalog's single writer transaction.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if an object is invalid, an edge target is
    /// absent, persisted content conflicts, or the actor is unavailable.
    pub fn store_edit_object_pack(
        &self,
        request: &EditObjectPackWrite,
    ) -> Result<StoreEditObjectPackResult, CatalogError> {
        self.request(|response| Message::StoreEditObjectPack(Box::new(request.clone()), response))
    }

    /// Reads and integrity-checks one content-addressed edit object.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if persisted bytes or indexed edges are corrupt
    /// or the actor is unavailable.
    pub fn edit_object(&self, id: EditObjectId) -> Result<Option<EditObjectRecord>, CatalogError> {
        self.request(|response| Message::EditObject(id, response))
    }

    /// Writes one Library-wide immutable edit commit and advances guarded refs
    /// in the same Catalog transaction.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for missing roots/parents, stale ref heads,
    /// content collisions, or an unavailable actor.
    pub fn commit_edit_repository(
        &self,
        request: &CommitEditRepository,
    ) -> Result<EditRepositoryCommitRecord, CatalogError> {
        self.request(|response| Message::CommitEditRepository(Box::new(request.clone()), response))
    }

    /// Atomically publishes one per-photo compatibility commit and its
    /// Library-wide repository commit through the single writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if either commit/ref set is invalid or stale,
    /// persistence fails, or the actor is unavailable.
    pub fn commit_recipe_and_edit_repository(
        &self,
        request: &CommitRecipeAndEditRepository,
    ) -> Result<CommitRecipeAndEditRepositoryResult, CatalogError> {
        self.request(|response| {
            Message::CommitRecipeAndEditRepository(Box::new(request.clone()), response)
        })
    }

    /// Reads and integrity-checks one Library-wide edit commit.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when persisted data is invalid or the actor is
    /// unavailable.
    pub fn edit_repository_commit(
        &self,
        id: EditCommitId,
    ) -> Result<Option<EditRepositoryCommitRecord>, CatalogError> {
        self.request(|response| Message::EditRepositoryCommit(id, response))
    }

    /// Resolves one guarded Library-wide branch, named version, or tag.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name, invalid persisted data, or
    /// an unavailable actor.
    pub fn edit_repository_ref(
        &self,
        name: &str,
    ) -> Result<Option<EditRepositoryRefRecord>, CatalogError> {
        self.request(|response| Message::EditRepositoryRef(name.to_owned(), response))
    }

    /// Returns one photo's current authoritative culling/rating decision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable, the photo is
    /// absent, or its current pointer is invalid.
    pub fn photo_decision_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoDecisionState, CatalogError> {
        self.request(|response| Message::Decision(DecisionMessage::State(photo_id, response)))
    }

    /// Appends one decision event and advances the current pointer atomically.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid input, a stale expected head,
    /// unavailable actor, or durable persistence failure.
    pub fn append_photo_decision_event(
        &self,
        request: &NewPhotoDecisionEvent,
    ) -> Result<PhotoDecisionEvent, CatalogError> {
        self.request(|response| {
            Message::Decision(DecisionMessage::Append(Box::new(request.clone()), response))
        })
    }

    /// Reads one bounded ascending page of immutable decisions for a photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable, the photo is
    /// absent, the bound is invalid, or history integrity checks fail.
    pub fn photo_decision_events_after(
        &self,
        photo_id: PhotoId,
        after_sequence_exclusive: u64,
        limit: usize,
    ) -> Result<PhotoDecisionPage, CatalogError> {
        self.request(|response| {
            Message::Decision(DecisionMessage::EventsAfter(
                photo_id,
                after_sequence_exclusive,
                limit,
                response,
            ))
        })
    }

    /// Appends one validated human-feedback event and returns its assigned sequence.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when validation, referenced-photo checks, or
    /// durable persistence fails.
    pub fn append_feedback_event(
        &self,
        request: &NewFeedbackEvent,
    ) -> Result<FeedbackEvent, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::AppendEvent(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Reads one bounded, ascending page after an exclusive sequence cursor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid page bound, unavailable actor,
    /// or persisted integrity failure.
    pub fn feedback_events_after(
        &self,
        scope: &LearningScope,
        after_sequence_exclusive: u64,
        limit: usize,
    ) -> Result<FeedbackPage, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::EventsAfter(
                scope.clone(),
                after_sequence_exclusive,
                limit,
                response,
            ))
        })
    }

    /// Appends a non-destructive forget fact for one existing feedback event.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when validation or persistence fails.
    pub fn append_feedback_forget_fact(
        &self,
        request: &NewFeedbackForgetFact,
    ) -> Result<FeedbackForgetFact, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::AppendForgetFact(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Returns every forgotten source event id in exactly one learning scope.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the query fails.
    pub fn forgotten_feedback_event_ids(
        &self,
        scope: &LearningScope,
    ) -> Result<BTreeSet<String>, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::ForgottenEventIds(scope.clone(), response))
        })
    }

    /// Lists resumable import sessions.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn unfinished_import_sessions(&self) -> Result<Vec<ImportSession>, CatalogError> {
        self.request(Message::UnfinishedImportSessions)
    }

    /// Returns the durable summary for one import session.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the session is
    /// absent, or the query fails.
    pub fn import_session_summary(
        &self,
        id: ImportSessionId,
    ) -> Result<ImportSessionSummary, CatalogError> {
        self.request(|response| Message::ImportSessionSummary(id, response))
    }

    fn request<T: Send + 'static>(
        &self,
        message: impl FnOnce(SyncSender<Result<T, CatalogError>>) -> Message,
    ) -> Result<T, CatalogError> {
        let (response_sender, response_receiver) = mpsc::sync_channel(0);
        self.sender
            .send(message(response_sender))
            .map_err(|_| CatalogError::ActorUnavailable)?;
        response_receiver
            .recv()
            .map_err(|_| CatalogError::ActorUnavailable)?
    }
}

impl CatalogStore for CatalogHandle {
    fn begin_import_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        self.request(|response| Message::BeginImportSession(root.clone(), now_ms, response))
    }

    fn resume_import_session(
        &mut self,
        id: ImportSessionId,
        now_ms: i64,
    ) -> Result<ImportSession, CatalogError> {
        self.request(|response| Message::ResumeImportSession(id, now_ms, response))
    }

    fn record_import_discovered(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::RecordImportDiscovered(session_id, request.clone(), response)
        })
    }

    fn register_import_asset(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| Message::RegisterImportAsset(session_id, request.clone(), response))
    }

    fn record_import_issue(
        &mut self,
        session_id: ImportSessionId,
        location: &AssetLocation,
        message: &str,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::RecordImportIssue(
                session_id,
                location.clone(),
                message.to_owned(),
                now_ms,
                response,
            )
        })
    }

    fn finish_import_session(
        &mut self,
        id: ImportSessionId,
        state: ImportSessionState,
        last_error: Option<&str>,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::FinishImportSession(id, state, last_error.map(str::to_owned), now_ms, response)
        })
    }
}

#[allow(clippy::too_many_lines)]
fn run_actor(mut catalog: Catalog, receiver: &Receiver<Message>) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::SchemaVersion(response) => respond(&response, catalog.schema_version()),
            Message::Stats(response) => respond(&response, catalog.stats()),
            Message::RegisterAsset(request, response) => {
                let _ = response.send(catalog.register_asset(&request));
            }
            Message::RegisterAssetWithContentIdentity(request, identity, response) => {
                let _ = response
                    .send(catalog.register_asset_with_content_identity(&request, &identity));
            }
            Message::RecordRepresentationContentIdentity(
                representation_id,
                identity,
                observed_at_ms,
                response,
            ) => {
                let _ = response.send(catalog.record_representation_content_identity(
                    representation_id,
                    &identity,
                    observed_at_ms,
                ));
            }
            Message::UpsertPhotoLibraryFacts(facts, response) => {
                let _ = response.send(catalog.upsert_photo_library_facts(facts.as_ref()));
            }
            Message::PhotoLibraryFacts(photo_id, response) => {
                let _ = response.send(catalog.photo_library_facts(photo_id));
            }
            Message::SetPhotoLibraryState(state, response) => {
                let _ = response.send(catalog.set_photo_library_state(state.as_ref()));
            }
            Message::PhotoLibraryState(photo_id, response) => {
                let _ = response.send(catalog.photo_library_state(photo_id));
            }
            Message::CreateLibraryAlbum(kind, name, query_json, now_ms, response) => {
                let _ = response.send(catalog.create_library_album(
                    kind,
                    &name,
                    query_json.as_deref(),
                    now_ms,
                ));
            }
            Message::LibraryAlbums(response) => {
                let _ = response.send(catalog.library_albums());
            }
            Message::AddPhotoToAlbum(album_id, photo_id, sort_key, now_ms, response) => {
                let _ =
                    response.send(catalog.add_photo_to_album(album_id, photo_id, sort_key, now_ms));
            }
            Message::RemovePhotoFromAlbum(album_id, photo_id, response) => {
                let _ = response.send(catalog.remove_photo_from_album(album_id, photo_id));
            }
            Message::AlbumsForPhoto(photo_id, response) => {
                let _ = response.send(catalog.albums_for_photo(photo_id));
            }
            Message::LibrarySources(response) => {
                let _ = response.send(catalog.library_sources());
            }
            Message::LibraryPhotoPage(filter, after, requested_limit, response) => {
                let _ = response.send(catalog.library_photo_page(
                    &filter,
                    after.as_ref(),
                    requested_limit,
                ));
            }
            Message::LibraryPhotoCount(filter, response) => {
                let _ = response.send(catalog.library_photo_count(&filter));
            }
            Message::RepresentationFingerprint(representation_id, response) => {
                let _ = response.send(catalog.representation_fingerprint(representation_id));
            }
            Message::RecordDecodeSnapshot(request, response) => {
                let _ = response.send(catalog.record_decode_snapshot(request.as_ref()));
            }
            Message::DecodeSnapshots(representation_id, response) => {
                let _ = response.send(catalog.decode_snapshots(representation_id));
            }
            Message::IsDecodeOutputCurrent(
                representation_id,
                provider_id,
                provider_version,
                source,
                require_cached_preview,
                proxy_variant_key,
                required_technical_preprocessing,
                response,
            ) => {
                let _ = response.send(catalog.is_decode_output_current(
                    representation_id,
                    &provider_id,
                    &provider_version,
                    source,
                    require_cached_preview,
                    &proxy_variant_key,
                    required_technical_preprocessing.as_deref(),
                ));
            }
            Message::RecordCachedArtifact(request, response) => {
                let _ = response.send(catalog.record_cached_artifact(request.as_ref()));
            }
            Message::CachedArtifacts(representation_id, response) => {
                let _ = response.send(catalog.cached_artifacts(representation_id));
            }
            Message::PreferredCachedArtifact(representation_id, response) => {
                let _ = response.send(catalog.preferred_cached_artifact(representation_id));
            }
            Message::InvalidateCachedArtifact(record, response) => {
                let _ = response.send(catalog.invalidate_cached_artifact(record.as_ref()));
            }
            Message::RecordTechnicalObservation(request, response) => {
                let _ = response.send(catalog.record_technical_observation(request.as_ref()));
            }
            Message::TechnicalObservation(
                representation_id,
                source,
                artifact,
                revision,
                response,
            ) => {
                let _ = response.send(catalog.technical_observation(
                    representation_id,
                    source,
                    artifact.as_ref(),
                    &revision,
                ));
            }
            Message::ReviewPage(after, limit, revision, recipe_preview_generator, response) => {
                let result = match (revision.as_ref(), recipe_preview_generator.as_ref()) {
                    (Some(revision), Some(recipe_preview_generator)) => catalog
                        .review_page_with_technical_and_recipe_preview_generator(
                            after.as_ref(),
                            limit,
                            revision,
                            recipe_preview_generator,
                        ),
                    (Some(revision), None) => {
                        catalog.review_page_with_technical(after.as_ref(), limit, revision)
                    }
                    (None, None) => catalog.review_page(after.as_ref(), limit),
                    (None, Some(_)) => unreachable!(
                        "a Recipe-preview generator filter requires a technical Review query"
                    ),
                };
                let _ = response.send(result);
            }
            Message::ReviewSource(photo_id, revision, response) => {
                let result = revision.as_ref().map_or_else(
                    || catalog.review_source(photo_id),
                    |revision| catalog.review_source_with_technical(photo_id, revision),
                );
                let _ = response.send(result);
            }
            Message::PhotoSource(photo_id, response) => {
                let _ = response.send(catalog.photo_source(photo_id));
            }
            Message::CommitRecipe(request, response) => {
                let _ = response.send(catalog.commit_recipe(request.as_ref()));
            }
            Message::RecipeCommits(photo_id, response) => {
                let _ = response.send(catalog.recipe_commits(photo_id));
            }
            Message::RecipeCommit(photo_id, commit_id, response) => {
                respond(&response, catalog.recipe_commit(photo_id, commit_id));
            }
            Message::RecipeRef(photo_id, name, response) => {
                let _ = response.send(catalog.recipe_ref(photo_id, &name));
            }
            Message::SetRecipeRef(request, response) => {
                let _ = response.send(catalog.set_recipe_ref(request.as_ref()));
            }
            Message::DiscardRecipeHistory(photo_id, response) => {
                let _ = response.send(catalog.discard_recipe_history(photo_id));
            }
            Message::StoreEditObjectPack(request, response) => {
                let _ = response.send(catalog.store_edit_object_pack(request.as_ref()));
            }
            Message::EditObject(id, response) => {
                respond(&response, catalog.edit_object(id));
            }
            Message::CommitEditRepository(request, response) => {
                let _ = response.send(catalog.commit_edit_repository(request.as_ref()));
            }
            Message::CommitRecipeAndEditRepository(request, response) => {
                let _ = response.send(catalog.commit_recipe_and_edit_repository(request.as_ref()));
            }
            Message::EditRepositoryCommit(id, response) => {
                respond(&response, catalog.edit_repository_commit(id));
            }
            Message::EditRepositoryRef(name, response) => {
                let _ = response.send(catalog.edit_repository_ref(&name));
            }
            Message::Decision(message) => run_decision_message(&mut catalog, message),
            Message::Feedback(message) => run_feedback_message(&mut catalog, message),
            Message::BeginImportSession(root, now_ms, response) => {
                let _ = response.send(catalog.begin_import_session(&root, now_ms));
            }
            Message::ResumeImportSession(id, now_ms, response) => {
                let _ = response.send(catalog.resume_import_session(id, now_ms));
            }
            Message::RecordImportDiscovered(id, request, response) => {
                let _ = response.send(catalog.record_import_discovered(id, &request));
            }
            Message::RegisterImportAsset(id, request, response) => {
                let _ = response.send(catalog.register_import_asset(id, &request));
            }
            Message::RecordImportIssue(id, location, message, now_ms, response) => {
                let _ = response.send(catalog.record_import_issue(id, &location, &message, now_ms));
            }
            Message::FinishImportSession(id, state, error, now_ms, response) => {
                let _ = response.send(catalog.finish_import_session(
                    id,
                    state,
                    error.as_deref(),
                    now_ms,
                ));
            }
            Message::ImportSessionSummary(id, response) => {
                let _ = response.send(catalog.import_session_summary(id));
            }
            Message::UnfinishedImportSessions(response) => {
                let _ = response.send(catalog.unfinished_import_sessions());
            }
            Message::Shutdown(response) => {
                let _ = response.send(());
                break;
            }
        }
    }
}

fn run_decision_message(catalog: &mut Catalog, message: DecisionMessage) {
    match message {
        DecisionMessage::State(photo_id, response) => {
            let _ = response.send(catalog.photo_decision_state(photo_id));
        }
        DecisionMessage::Append(request, response) => {
            let _ = response.send(catalog.append_photo_decision_event(request.as_ref()));
        }
        DecisionMessage::EventsAfter(photo_id, after_sequence, limit, response) => {
            let _ =
                response.send(catalog.photo_decision_events_after(photo_id, after_sequence, limit));
        }
    }
}

fn respond<T>(sender: &SyncSender<Result<T, CatalogError>>, result: Result<T, CatalogError>) {
    let _ = sender.send(result);
}

fn run_feedback_message(catalog: &mut Catalog, message: FeedbackMessage) {
    match message {
        FeedbackMessage::AppendEvent(request, response) => {
            let _ = response.send(catalog.append_feedback_event(request.as_ref()));
        }
        FeedbackMessage::EventsAfter(scope, after_sequence, limit, response) => {
            let _ = response.send(catalog.feedback_events_after(&scope, after_sequence, limit));
        }
        FeedbackMessage::AppendForgetFact(request, response) => {
            let _ = response.send(catalog.append_feedback_forget_fact(request.as_ref()));
        }
        FeedbackMessage::ForgottenEventIds(scope, response) => {
            let _ = response.send(catalog.forgotten_feedback_event_ids(&scope));
        }
    }
}

#[cfg(test)]
mod tests {
    use std::{
        sync::{Arc, Barrier},
        thread,
    };

    use shadow_ai::{FeedbackAction, PresentationContext};
    use shadow_domain::{
        EditEntityEntryV1, EditEntityMapV1, EditObject, EditObjectKind, EditObjectPack,
        EditRepositoryCommit, EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation,
        EditRepositoryRefKind, EntityId, LibraryRootV1, PhotoDecisionOrigin, PhotoFlag, Platform,
        RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind,
    };

    use crate::EditRepositoryRefUpdate;

    use super::*;

    #[test]
    fn cloned_handles_serialize_writes_through_one_actor() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handles = (0_u8..4)
            .map(|index| {
                let handle = actor.handle();
                thread::spawn(move || {
                    let request = RegisterAsset {
                        kind: RepresentationKind::OriginalRaw,
                        location: AssetLocation::new(
                            Platform::MacOs,
                            format!("/photos/{index}.nef").into_bytes(),
                            format!("/photos/{index}.nef"),
                        ),
                        byte_len: 42,
                        modified_at_ms: Some(100),
                        now_ms: 1_700_000_000_000,
                    };
                    handle.register_asset(&request).expect("register asset");
                })
            })
            .collect::<Vec<_>>();

        for handle in handles {
            handle.join().expect("join client thread");
        }
        assert_eq!(actor.handle().stats().expect("stats").photos, 4);
        actor.shutdown().expect("shutdown actor");
    }

    #[test]
    fn actor_pages_photo_first_library_rows() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handle = actor.handle();
        let registered = handle
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/library-page.dng".to_vec(),
                    "/photos/library-page.dng",
                ),
                byte_len: 42,
                modified_at_ms: Some(100),
                now_ms: 1_700_000_000_000,
            })
            .expect("register library photo");
        let page = handle
            .library_photo_page(&LibraryPhotoFilter::default(), None, 16)
            .expect("page Library through actor");
        assert_eq!(
            handle
                .library_photo_count(&LibraryPhotoFilter::default())
                .expect("count Library through actor"),
            1
        );
        assert_eq!(page.items[0].photo_id, registered.photo_id);
        assert_eq!(
            page.items[0].location.display_path,
            "/photos/library-page.dng"
        );
        actor.shutdown().expect("shutdown actor");
    }

    #[test]
    fn actor_resolves_an_original_raster_through_the_source_neutral_query() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handle = actor.handle();
        let registered = handle
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaster,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/editable.jpg".to_vec(),
                    "/photos/editable.jpg",
                ),
                byte_len: 42,
                modified_at_ms: Some(100),
                now_ms: 1_700_000_000_000,
            })
            .expect("register raster photo");

        assert_eq!(
            handle
                .photo_source(registered.photo_id)
                .expect("resolve source-neutral photo source")
                .expect("online raster source")
                .representation_id,
            registered.representation_id
        );
        assert!(
            handle
                .review_source(registered.photo_id)
                .expect("resolve legacy RAW-only source")
                .is_none()
        );
        actor.shutdown().expect("shutdown actor");
    }

    #[test]
    fn actor_resolves_exact_recipe_commits_without_crossing_photo_owners() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handle = actor.handle();
        let register = |path: &str| RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        };
        let owner = handle
            .register_asset(&register("/photos/exact-owner.dng"))
            .expect("register commit owner");
        let other = handle
            .register_asset(&register("/photos/exact-other.dng"))
            .expect("register other photo");
        let commit = RecipeCommit::new(
            RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            Vec::new(),
            RecipeSnapshot::empty(),
            Some("Exact actor lookup".to_owned()),
            1_700_000_001_000,
        )
        .expect("build Recipe commit");
        let expected = handle
            .commit_recipe(&CommitRecipe {
                photo_id: owner.photo_id,
                commit: commit.clone(),
                update_refs: Vec::new(),
            })
            .expect("commit Recipe through actor");

        assert_eq!(
            handle
                .recipe_commit(owner.photo_id, commit.id())
                .expect("resolve exact commit through actor"),
            Some(expected)
        );
        assert!(
            handle
                .recipe_commit(other.photo_id, commit.id())
                .expect("query commit through the wrong owner")
                .is_none()
        );
        assert!(
            handle
                .recipe_commit(owner.photo_id, RecipeCommitId::new_v7())
                .expect("query absent exact commit through actor")
                .is_none()
        );

        actor.shutdown().expect("shutdown actor");
    }

    #[test]
    fn actor_serializes_library_object_pack_commit_and_ref() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handle = actor.handle();
        let photo = EditObject::from_canonical_json(
            EditObjectKind::PhotoEditState,
            1,
            &serde_json::json!({ "photo": "actor-photo" }),
        )
        .expect("build photo object");
        let photo = EditObjectPack::new(photo, Vec::new()).expect("pack photo object");
        let photo_map = EditEntityMapV1::new(vec![EditEntityEntryV1 {
            key: "photo/actor-photo".into(),
            value: photo.object().id(),
        }])
        .expect("build photo map")
        .into_object_pack()
        .expect("pack photo map");
        let root = LibraryRootV1 {
            photo_recipes: Some(photo_map.object().id()),
            shared_grade_heads: None,
            masks: None,
            styles: None,
            output_states: None,
        }
        .into_object_pack()
        .expect("pack Library root");
        let root_id = root.object().id();
        assert_eq!(
            handle
                .store_edit_object_pack(&EditObjectPackWrite {
                    objects: vec![root, photo_map, photo],
                    created_at_ms: 10,
                })
                .expect("store object pack")
                .inserted,
            3
        );
        let commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
            root: root_id,
            parents: Vec::new(),
            message: Some("Actor Library checkpoint".into()),
            created_at_ms: 11,
        })
        .expect("build Library commit");
        handle
            .commit_edit_repository(&CommitEditRepository {
                commit: commit.clone(),
                update_refs: vec![EditRepositoryRefUpdate {
                    name: "heads/main".into(),
                    kind: EditRepositoryRefKind::Branch,
                    expected: EditRepositoryRefExpectation::Missing,
                    updated_at_ms: 11,
                }],
            })
            .expect("commit Library state");

        assert_eq!(
            handle
                .edit_repository_commit(commit.id())
                .expect("read Library commit")
                .expect("Library commit exists")
                .commit,
            commit
        );
        assert_eq!(
            handle
                .edit_repository_ref("heads/main")
                .expect("read Library head")
                .expect("Library head exists")
                .commit_id,
            commit.id()
        );
        assert_eq!(
            handle
                .edit_object(root_id)
                .expect("read Library root")
                .expect("Library root exists")
                .object
                .id(),
            root_id
        );
        actor.shutdown().expect("shutdown actor");
    }

    #[test]
    fn actor_pages_feedback_and_appends_forget_facts() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handle = actor.handle();
        let registered = handle
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/feedback-actor.dng".to_vec(),
                    "/photos/feedback-actor.dng",
                ),
                byte_len: 42,
                modified_at_ms: Some(100),
                now_ms: 1_700_000_000_000,
            })
            .expect("register asset");
        for event_id in ["actor-event-1", "actor-event-2"] {
            handle
                .append_feedback_event(&NewFeedbackEvent {
                    event_id: event_id.into(),
                    occurred_at_unix_ms: 1_700_000_001_000,
                    scope: LearningScope::Global,
                    presentation: PresentationContext {
                        session_id: "actor-session".into(),
                        group_id: None,
                        candidates: vec![],
                        active_model: None,
                    },
                    action: FeedbackAction::Exported {
                        photo_id: registered.photo_id,
                    },
                })
                .expect("append feedback through actor");
        }

        let first_page = handle
            .feedback_events_after(&LearningScope::Global, 0, 1)
            .expect("page feedback through actor");
        assert!(first_page.has_more);
        assert_eq!(first_page.events[0].event_id, "actor-event-1");
        handle
            .append_feedback_forget_fact(&NewFeedbackForgetFact {
                fact_id: "actor-forget-1".into(),
                target_event_id: "actor-event-1".into(),
                occurred_at_unix_ms: 1_700_000_002_000,
                reason: None,
            })
            .expect("append forget through actor");
        assert_eq!(
            handle
                .forgotten_feedback_event_ids(&LearningScope::Global)
                .expect("read forgotten through actor"),
            BTreeSet::from(["actor-event-1".into()])
        );
        assert_eq!(
            handle
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("source facts remain")
                .events
                .len(),
            2
        );
        actor.shutdown().expect("shutdown actor");
    }

    #[test]
    fn concurrent_decision_cas_allows_exactly_one_writer_to_advance_the_head() {
        let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
        let handle = actor.handle();
        let registered = handle
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/concurrent-decision.dng".to_vec(),
                    "/photos/concurrent-decision.dng",
                ),
                byte_len: 42,
                modified_at_ms: Some(100),
                now_ms: 1_700_000_000_000,
            })
            .expect("register concurrent decision photo");
        let barrier = Arc::new(Barrier::new(2));
        let workers = [
            ("concurrent-pick", PhotoFlag::Picked),
            ("concurrent-reject", PhotoFlag::Rejected),
        ]
        .into_iter()
        .map(|(event_id, after_flag)| {
            let handle = handle.clone();
            let barrier = Arc::clone(&barrier);
            let photo_id = registered.photo_id;
            thread::spawn(move || {
                barrier.wait();
                handle.append_photo_decision_event(&NewPhotoDecisionEvent {
                    event_id: event_id.into(),
                    photo_id,
                    occurred_at_unix_ms: 1_700_000_001_000,
                    origin: PhotoDecisionOrigin::Human,
                    expected_head_sequence: 0,
                    before_flag: PhotoFlag::Unflagged,
                    before_rating: 0,
                    after_flag,
                    after_rating: 0,
                })
            })
        })
        .collect::<Vec<_>>();
        let results = workers
            .into_iter()
            .map(|worker| worker.join().expect("decision worker panicked"))
            .collect::<Vec<_>>();
        assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(
            results
                .iter()
                .filter(|result| matches!(
                    result,
                    Err(CatalogError::PhotoDecisionHeadMismatch { .. })
                ))
                .count(),
            1
        );
        let winner = results
            .into_iter()
            .find_map(Result::ok)
            .expect("one winning decision");
        assert_eq!(
            handle
                .photo_decision_state(registered.photo_id)
                .expect("read winning decision"),
            winner.after_state().expect("winning state")
        );
        assert_eq!(
            handle
                .photo_decision_events_after(registered.photo_id, 0, 10)
                .expect("read concurrent decision history")
                .events,
            [winner]
        );
        actor.shutdown().expect("shutdown actor");
    }
}
