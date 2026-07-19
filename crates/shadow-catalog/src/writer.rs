use std::{
    path::Path,
    sync::mpsc::{self, Receiver, Sender, SyncSender},
    thread::{self, JoinHandle},
};

use shadow_domain::{AssetLocation, ImportSessionId};

use crate::{
    Catalog, CatalogError, CatalogStats, CatalogStore, ImportSession, ImportSessionState,
    ImportSessionSummary, RegisterAsset, RegisteredAsset,
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
            Message::SchemaVersion(response) => {
                let _ = response.send(catalog.schema_version());
            }
            Message::Stats(response) => {
                let _ = response.send(catalog.stats());
            }
            Message::RegisterAsset(request, response) => {
                let _ = response.send(catalog.register_asset(&request));
            }
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

#[cfg(test)]
mod tests {
    use std::thread;

    use shadow_domain::{Platform, RepresentationKind};

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
}
