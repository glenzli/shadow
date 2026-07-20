use std::{
    collections::BTreeSet,
    path::Path,
    sync::mpsc::{self, Receiver, Sender, SyncSender},
    thread::{self, JoinHandle},
};

use shadow_ai::{
    FeedbackEvent, FeedbackForgetFact, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact,
};
use shadow_domain::{AssetLocation, ImportSessionId, PhotoId, RecipeCommitId, RepresentationId};

use crate::{
    CachedArtifactRecord, Catalog, CatalogError, CatalogStats, CatalogStore, CommitRecipe,
    DecodeSnapshotRecord, FeedbackPage, ImportSession, ImportSessionState, ImportSessionSummary,
    InvalidateCachedArtifactStatus, RecipeCommitRecord, RecipeRefRecord, RecordCachedArtifact,
    RecordCachedArtifactStatus, RecordDecodeSnapshot, RecordDecodeSnapshotStatus, RegisterAsset,
    RegisteredAsset, RepresentationFingerprint, ReviewCursor, ReviewItemRecord, ReviewPageRecord,
    SetRecipeRef,
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
    InvalidateCachedArtifact(
        Box<CachedArtifactRecord>,
        SyncSender<Result<InvalidateCachedArtifactStatus, CatalogError>>,
    ),
    ReviewPage(
        Option<ReviewCursor>,
        usize,
        SyncSender<Result<ReviewPageRecord, CatalogError>>,
    ),
    ReviewSource(
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
    pub fn is_decode_output_current(
        &self,
        representation_id: RepresentationId,
        provider_id: &str,
        provider_version: &str,
        source: RepresentationFingerprint,
        require_cached_preview: bool,
        proxy_variant_key: &str,
    ) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::IsDecodeOutputCurrent(
                representation_id,
                provider_id.to_owned(),
                provider_version.to_owned(),
                source,
                require_cached_preview,
                proxy_variant_key.to_owned(),
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
        self.request(|response| Message::ReviewPage(after.cloned(), limit, response))
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
        self.request(|response| Message::ReviewSource(photo_id, response))
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

fn run_actor(mut catalog: Catalog, receiver: &Receiver<Message>) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::SchemaVersion(response) => respond(&response, catalog.schema_version()),
            Message::Stats(response) => respond(&response, catalog.stats()),
            Message::RegisterAsset(request, response) => {
                let _ = response.send(catalog.register_asset(&request));
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
                response,
            ) => {
                let _ = response.send(catalog.is_decode_output_current(
                    representation_id,
                    &provider_id,
                    &provider_version,
                    source,
                    require_cached_preview,
                    &proxy_variant_key,
                ));
            }
            Message::RecordCachedArtifact(request, response) => {
                let _ = response.send(catalog.record_cached_artifact(request.as_ref()));
            }
            Message::CachedArtifacts(representation_id, response) => {
                let _ = response.send(catalog.cached_artifacts(representation_id));
            }
            Message::InvalidateCachedArtifact(record, response) => {
                let _ = response.send(catalog.invalidate_cached_artifact(record.as_ref()));
            }
            Message::ReviewPage(after, limit, response) => {
                let _ = response.send(catalog.review_page(after.as_ref(), limit));
            }
            Message::ReviewSource(photo_id, response) => {
                let _ = response.send(catalog.review_source(photo_id));
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
    use std::thread;

    use shadow_ai::{FeedbackAction, PresentationContext};
    use shadow_domain::{
        EntityId, Platform, RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind,
    };

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
}
