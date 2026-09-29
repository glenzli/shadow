//! Managed remote-Library server lifecycle for desktop and operator clients.
//!
//! This owner keeps provider discovery, shared-root scanning, authenticated listener lifetime,
//! cache inventory, and reset policy together. UI and CLI facades supply configuration but never
//! own a server thread or private decoder lifecycle.

mod provider_host;

use std::{
    collections::HashSet,
    fmt, fs,
    net::SocketAddr,
    path::{Path, PathBuf},
    sync::{Arc, Mutex, RwLock},
    thread::{self, JoinHandle},
};

use anyhow::{Context, Result, anyhow, bail};
use shadow_catalog::{CatalogActor, CatalogError};
use shadow_core::{
    DecodeInspectionActor, ScanCancellation, ScanCompletion, scan_folder_with_inspection_controlled,
};
use shadow_library_sharing::{
    AuthorizationToken, CatalogSharePolicy, CatalogShareSource, LibraryServer, LibraryServerConfig,
    LibraryShareSource, RunningLibraryServer,
    protocol::{
        MAX_LIBRARY_PAGE_SIZE, OriginalChunk, PreparedOriginal, RemoteError, RemotePhotoPage,
        RemotePreviewManifest, ServerInfo,
    },
};
use uuid::Uuid;

use self::provider_host::LibraryServerPreviewRuntime;

#[derive(Debug, Clone)]
pub struct LibraryServerStorage {
    pub catalog_path: PathBuf,
    pub preview_cache_root: PathBuf,
    pub state_root: PathBuf,
}

impl LibraryServerStorage {
    #[must_use]
    pub fn for_root(root: impl Into<PathBuf>) -> Self {
        let root = root.into();
        Self {
            catalog_path: root.join("catalog.sqlite"),
            preview_cache_root: root.join("preview-cache"),
            state_root: root.join("state"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct LibraryServerStartRequest {
    pub bind_address: SocketAddr,
    pub authorization: AuthorizationToken,
    pub display_name: String,
    pub share_roots: Vec<PathBuf>,
    pub serves_originals: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryServerSnapshot {
    pub running: bool,
    pub local_address: Option<SocketAddr>,
    pub display_name: String,
    pub provider_mode: String,
    pub photo_count: u64,
    pub cache_byte_len: u64,
    pub shared_root_count: u64,
    pub serves_originals: bool,
    pub index_state: String,
    pub discovered_file_count: u64,
    pub inspection_completed_count: u64,
    pub published_preview_count: u64,
    pub index_diagnostic: String,
}

#[derive(Debug)]
struct RunningState {
    server: RunningLibraryServer,
    display_name: String,
    provider_mode: String,
    shared_root_count: u64,
    serves_originals: bool,
    index_job: IndexJob,
}

#[derive(Debug, Default)]
struct ServiceState {
    starting: bool,
    running: Option<RunningState>,
    last_snapshot: Option<LibraryServerSnapshot>,
}

pub struct LibraryServerService {
    storage: LibraryServerStorage,
    state: Mutex<ServiceState>,
    #[cfg(test)]
    indexing_hook: Option<IndexingHook>,
}

impl fmt::Debug for LibraryServerService {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("LibraryServerService")
            .field("storage", &self.storage)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
type IndexingHook = Arc<dyn Fn(&ScanCancellation) -> Result<()> + Send + Sync>;

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
enum IndexState {
    Scanning,
    Ready,
    Failed,
    Cancelled,
}

impl IndexState {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Scanning => "scanning",
            Self::Ready => "ready",
            Self::Failed => "failed",
            Self::Cancelled => "cancelled",
        }
    }
}

#[derive(Debug, Clone)]
struct IndexProgress {
    state: IndexState,
    discovered_files: u64,
    inspections_completed: u64,
    published_previews: u64,
    published_photo_count: u64,
    diagnostic: String,
}

impl IndexProgress {
    fn scanning(published_photo_count: u64) -> Self {
        Self {
            state: IndexState::Scanning,
            discovered_files: 0,
            inspections_completed: 0,
            published_previews: 0,
            published_photo_count,
            diagnostic: String::new(),
        }
    }
}

#[derive(Debug)]
struct IndexJob {
    cancellation: ScanCancellation,
    progress: Arc<Mutex<IndexProgress>>,
    worker: Option<JoinHandle<()>>,
}

#[derive(Debug)]
struct PublishedCatalogSource {
    current: RwLock<PublishedCatalogs>,
}

#[derive(Debug)]
struct PublishedCatalogs {
    current: Arc<CatalogShareSource>,
    retired: Vec<Arc<CatalogShareSource>>,
}

impl PublishedCatalogSource {
    fn new(source: Arc<CatalogShareSource>) -> Self {
        Self {
            current: RwLock::new(PublishedCatalogs {
                current: source,
                retired: Vec::new(),
            }),
        }
    }

    fn publish(&self, source: Arc<CatalogShareSource>) {
        let mut sources = self
            .current
            .write()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let previous = std::mem::replace(&mut sources.current, source);
        sources.retired.push(previous);
        if sources.retired.len() > 2 {
            sources.retired.remove(0);
        }
    }

    fn sources(&self) -> Vec<Arc<CatalogShareSource>> {
        let sources = self
            .current
            .read()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        std::iter::once(Arc::clone(&sources.current))
            .chain(sources.retired.iter().rev().cloned())
            .collect()
    }
}

impl LibraryShareSource for PublishedCatalogSource {
    fn server_info(&self) -> ServerInfo {
        self.sources()[0].server_info()
    }

    fn list_photos(
        &self,
        cursor: Option<&str>,
        limit: u16,
    ) -> Result<RemotePhotoPage, RemoteError> {
        if cursor.is_none() {
            return self.sources()[0].list_photos(None, limit);
        }
        first_success(self.sources(), |source| source.list_photos(cursor, limit))
    }

    fn fetch_preview(
        &self,
        digest_blake3: [u8; 32],
    ) -> Result<(RemotePreviewManifest, Vec<u8>), RemoteError> {
        first_success(self.sources(), |source| source.fetch_preview(digest_blake3))
    }

    fn prepare_original(
        &self,
        photo_id: shadow_domain::PhotoId,
        representation_id: shadow_domain::RepresentationId,
    ) -> Result<PreparedOriginal, RemoteError> {
        first_success(self.sources(), |source| {
            source.prepare_original(photo_id, representation_id)
        })
    }

    fn read_original(
        &self,
        revision_token: &str,
        offset: u64,
        maximum_bytes: u32,
    ) -> Result<(OriginalChunk, Vec<u8>), RemoteError> {
        first_success(self.sources(), |source| {
            source.read_original(revision_token, offset, maximum_bytes)
        })
    }
}

fn first_success<T>(
    sources: Vec<Arc<CatalogShareSource>>,
    mut operation: impl FnMut(&CatalogShareSource) -> Result<T, RemoteError>,
) -> Result<T, RemoteError> {
    let mut last_error = None;
    for source in sources {
        match operation(&source) {
            Ok(value) => return Ok(value),
            Err(error) => last_error = Some(error),
        }
    }
    Err(last_error.expect("published Catalog source set is never empty"))
}

impl LibraryServerService {
    #[must_use]
    pub fn new(storage: LibraryServerStorage) -> Self {
        Self {
            storage,
            state: Mutex::new(ServiceState::default()),
            #[cfg(test)]
            indexing_hook: None,
        }
    }

    #[cfg(test)]
    fn new_with_indexing_hook(storage: LibraryServerStorage, indexing_hook: IndexingHook) -> Self {
        Self {
            storage,
            state: Mutex::new(ServiceState::default()),
            indexing_hook: Some(indexing_hook),
        }
    }

    /// Starts one authenticated listener, then builds a replacement Catalog in the background.
    ///
    /// # Errors
    ///
    /// Fails for invalid roots/configuration, provider preparation failures, published-Catalog
    /// errors, or listener admission failures. Background indexing failures are reported through
    /// [`Self::snapshot`] while the last complete Catalog remains available.
    pub fn start(&self, request: LibraryServerStartRequest) -> Result<LibraryServerSnapshot> {
        let display_name = request.display_name.trim().to_owned();
        if display_name.is_empty() {
            bail!("remote Library server name must not be empty");
        }
        let share_roots = normalized_share_roots(request.share_roots.clone())?;
        {
            let mut state = self.lock_state()?;
            if state.running.is_some() || state.starting {
                bail!("remote Library server is already running or starting");
            }
            state.starting = true;
        }
        let result = self.start_reserved(request, display_name, share_roots);
        if result.is_err()
            && let Ok(mut state) = self.lock_state()
        {
            state.starting = false;
        }
        result
    }

    fn start_reserved(
        &self,
        request: LibraryServerStartRequest,
        display_name: String,
        share_roots: Vec<PathBuf>,
    ) -> Result<LibraryServerSnapshot> {
        ensure_parent(&self.storage.catalog_path)?;
        fs::create_dir_all(&self.storage.preview_cache_root).with_context(|| {
            format!(
                "create remote Library preview cache {}",
                self.storage.preview_cache_root.display()
            )
        })?;
        fs::create_dir_all(&self.storage.state_root).with_context(|| {
            format!(
                "create remote Library state directory {}",
                self.storage.state_root.display()
            )
        })?;

        let preview_runtime =
            LibraryServerPreviewRuntime::discover(&self.storage.preview_cache_root)?;
        let private_provider_available = preview_runtime.private_provider_available();
        let provider_mode = preview_runtime.mode_label().to_owned();
        let (published_catalog_path, photo_count) = self.prepare_published_catalog()?;
        let policy = CatalogSharePolicy::for_roots(share_roots.clone(), request.serves_originals);
        let source = Arc::new(CatalogShareSource::open_with_policy(
            &published_catalog_path,
            &self.storage.preview_cache_root,
            &self.storage.state_root,
            &display_name,
            private_provider_available,
            policy.clone(),
        )?);
        let published_source = Arc::new(PublishedCatalogSource::new(source));
        let cache_byte_len = directory_byte_len(&self.storage.preview_cache_root)?;
        let server = LibraryServer::new(
            LibraryServerConfig::new(request.bind_address, request.authorization),
            Arc::clone(&published_source) as Arc<dyn LibraryShareSource>,
        )
        .start()?;
        let progress = Arc::new(Mutex::new(IndexProgress::scanning(photo_count)));
        let cancellation = ScanCancellation::new();
        let worker = match self.spawn_index_worker(IndexWorkerRequest {
            storage: self.storage.clone(),
            share_roots: share_roots.clone(),
            preview_runtime: Some(preview_runtime),
            display_name: display_name.clone(),
            private_provider_available,
            policy,
            published_source,
            progress: Arc::clone(&progress),
            cancellation: cancellation.clone(),
            #[cfg(test)]
            indexing_hook: self.indexing_hook.clone(),
        }) {
            Ok(worker) => worker,
            Err(error) => {
                let _ = server.shutdown();
                return Err(error);
            }
        };
        let snapshot = LibraryServerSnapshot {
            running: true,
            local_address: Some(server.local_address()),
            display_name: display_name.clone(),
            provider_mode: provider_mode.clone(),
            photo_count,
            cache_byte_len,
            shared_root_count: u64::try_from(share_roots.len()).unwrap_or(u64::MAX),
            serves_originals: request.serves_originals,
            index_state: IndexState::Scanning.as_str().to_owned(),
            discovered_file_count: 0,
            inspection_completed_count: 0,
            published_preview_count: 0,
            index_diagnostic: String::new(),
        };
        let mut state = self.lock_state()?;
        state.starting = false;
        state.running = Some(RunningState {
            server,
            display_name,
            provider_mode,
            shared_root_count: snapshot.shared_root_count,
            serves_originals: request.serves_originals,
            index_job: IndexJob {
                cancellation,
                progress,
                worker: Some(worker),
            },
        });
        state.last_snapshot = Some(snapshot.clone());
        Ok(snapshot)
    }

    /// Stops request admission and waits for already admitted clients to finish.
    ///
    /// # Errors
    ///
    /// Fails when the service is not running or its listener cannot shut down cleanly.
    pub fn stop(&self) -> Result<LibraryServerSnapshot> {
        let running = {
            let mut state = self.lock_state()?;
            if state.starting {
                bail!("remote Library server is still starting");
            }
            state
                .running
                .take()
                .ok_or_else(|| anyhow!("remote Library server is not running"))?
        };
        let RunningState {
            server,
            display_name,
            provider_mode,
            shared_root_count,
            serves_originals,
            mut index_job,
        } = running;
        index_job.cancellation.cancel();
        let shutdown_result = server.shutdown();
        let worker_result = join_index_worker(&mut index_job);
        shutdown_result?;
        worker_result?;
        let progress = lock_progress(&index_job.progress).clone();
        let generation_cleanup_error = prune_catalog_generations(&self.storage).err();
        let snapshot = LibraryServerSnapshot {
            running: false,
            local_address: None,
            display_name,
            provider_mode,
            photo_count: progress.published_photo_count,
            cache_byte_len: directory_byte_len(&self.storage.preview_cache_root)?,
            shared_root_count,
            serves_originals,
            index_state: progress.state.as_str().to_owned(),
            discovered_file_count: progress.discovered_files,
            inspection_completed_count: progress.inspections_completed,
            published_preview_count: progress.published_previews,
            index_diagnostic: generation_cleanup_error.map_or(
                progress.diagnostic.clone(),
                |error| {
                    if progress.diagnostic.is_empty() {
                        format!("generation cleanup failed: {error:#}")
                    } else {
                        format!(
                            "{}; generation cleanup failed: {error:#}",
                            progress.diagnostic
                        )
                    }
                },
            ),
        };
        self.lock_state()?.last_snapshot = Some(snapshot.clone());
        Ok(snapshot)
    }

    /// Returns current listener/cache state. When stopped, Provider discovery is refreshed so the
    /// settings UI can accurately explain whether private RAW previews will work before startup.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid configured Provider Host or unreadable cache directory.
    pub fn snapshot(&self) -> Result<LibraryServerSnapshot> {
        let mut state = self.lock_state()?;
        if let Some(running) = &mut state.running {
            join_finished_index_worker(&mut running.index_job);
            let progress = lock_progress(&running.index_job.progress).clone();
            return Ok(LibraryServerSnapshot {
                running: true,
                local_address: Some(running.server.local_address()),
                display_name: running.display_name.clone(),
                provider_mode: running.provider_mode.clone(),
                photo_count: progress.published_photo_count,
                cache_byte_len: directory_byte_len(&self.storage.preview_cache_root)?,
                shared_root_count: running.shared_root_count,
                serves_originals: running.serves_originals,
                index_state: progress.state.as_str().to_owned(),
                discovered_file_count: progress.discovered_files,
                inspection_completed_count: progress.inspections_completed,
                published_preview_count: progress.published_previews,
                index_diagnostic: progress.diagnostic,
            });
        }
        let previous = state.last_snapshot.clone();
        drop(state);
        let provider_mode =
            LibraryServerPreviewRuntime::discover(&self.storage.preview_cache_root)?
                .mode_label()
                .to_owned();
        Ok(LibraryServerSnapshot {
            running: false,
            local_address: None,
            display_name: previous
                .as_ref()
                .map_or_else(String::new, |snapshot| snapshot.display_name.clone()),
            provider_mode,
            photo_count: previous.as_ref().map_or(0, |snapshot| snapshot.photo_count),
            cache_byte_len: directory_byte_len(&self.storage.preview_cache_root)?,
            shared_root_count: previous
                .as_ref()
                .map_or(0, |snapshot| snapshot.shared_root_count),
            serves_originals: previous
                .as_ref()
                .is_none_or(|snapshot| snapshot.serves_originals),
            index_state: previous.as_ref().map_or_else(
                || "idle".to_owned(),
                |snapshot| snapshot.index_state.clone(),
            ),
            discovered_file_count: previous
                .as_ref()
                .map_or(0, |snapshot| snapshot.discovered_file_count),
            inspection_completed_count: previous
                .as_ref()
                .map_or(0, |snapshot| snapshot.inspection_completed_count),
            published_preview_count: previous
                .as_ref()
                .map_or(0, |snapshot| snapshot.published_preview_count),
            index_diagnostic: previous
                .as_ref()
                .map_or_else(String::new, |snapshot| snapshot.index_diagnostic.clone()),
        })
    }

    /// Clears only the rebuildable server Catalog and preview/runtime caches.
    /// Stable server identity and UI-owned configuration remain intact.
    ///
    /// # Errors
    ///
    /// Fails while running or when a bounded server-owned path cannot be removed.
    pub fn reset_cache(&self) -> Result<LibraryServerSnapshot> {
        let mut state = self.lock_state()?;
        if state.running.is_some() || state.starting {
            bail!("stop the remote Library server before clearing its cache");
        }
        for catalog_path in catalog_files(&self.storage.catalog_path) {
            remove_file_if_present(&catalog_path)?;
        }
        remove_directory_if_present(&self.storage.preview_cache_root)?;
        remove_directory_if_present(&catalog_generations_root(&self.storage))?;
        remove_file_if_present(&published_catalog_pointer(&self.storage))?;
        state.last_snapshot = None;
        drop(state);
        self.snapshot()
    }

    fn prepare_published_catalog(&self) -> Result<(PathBuf, u64)> {
        let published = read_published_catalog(&self.storage)?;
        let path = published.as_ref().map_or_else(
            || self.storage.catalog_path.clone(),
            |published| published.path.clone(),
        );
        let photo_count = published
            .and_then(|published| published.photo_count)
            .unwrap_or(0);
        prune_catalog_generations(&self.storage)?;
        self.open_catalog_actor_at(&path)?.shutdown()?;
        Ok((path, photo_count))
    }

    fn spawn_index_worker(&self, request: IndexWorkerRequest) -> Result<JoinHandle<()>> {
        thread::Builder::new()
            .name("shadow-library-server-index".to_owned())
            .spawn(move || run_index_worker(request))
            .context("spawn remote Library background index worker")
    }

    fn open_catalog_actor_at(&self, path: &Path) -> Result<CatalogActor> {
        open_catalog_actor_at(path)
    }

    #[cfg(test)]
    fn open_catalog_actor(&self) -> Result<CatalogActor> {
        self.open_catalog_actor_at(&self.storage.catalog_path)
    }

    fn lock_state(&self) -> Result<std::sync::MutexGuard<'_, ServiceState>> {
        self.state
            .lock()
            .map_err(|_| anyhow!("remote Library server state lock is poisoned"))
    }
}

struct IndexWorkerRequest {
    storage: LibraryServerStorage,
    share_roots: Vec<PathBuf>,
    preview_runtime: Option<LibraryServerPreviewRuntime>,
    display_name: String,
    private_provider_available: bool,
    policy: CatalogSharePolicy,
    published_source: Arc<PublishedCatalogSource>,
    progress: Arc<Mutex<IndexProgress>>,
    cancellation: ScanCancellation,
    #[cfg(test)]
    indexing_hook: Option<IndexingHook>,
}

fn run_index_worker(request: IndexWorkerRequest) {
    let progress = Arc::clone(&request.progress);
    let result = index_catalog_generation(request);
    let mut state = lock_progress(&progress);
    match result {
        Ok(Some(photo_count)) => {
            state.published_photo_count = photo_count;
            state.state = IndexState::Ready;
            state.diagnostic.clear();
        }
        Ok(None) => {
            state.state = IndexState::Cancelled;
            state.diagnostic.clear();
        }
        Err(error) => {
            state.state = IndexState::Failed;
            state.diagnostic = format!("{error:#}");
        }
    }
}

fn index_catalog_generation(mut request: IndexWorkerRequest) -> Result<Option<u64>> {
    #[cfg(test)]
    if let Some(hook) = &request.indexing_hook {
        hook(&request.cancellation)?;
    }
    if request.cancellation.is_cancelled() {
        return Ok(None);
    }
    let generation_id = Uuid::now_v7().to_string();
    let generation_root = catalog_generations_root(&request.storage).join(&generation_id);
    let catalog_path = generation_root.join("catalog.sqlite");
    fs::create_dir_all(&generation_root).with_context(|| {
        format!(
            "create remote Library Catalog generation {}",
            generation_root.display()
        )
    })?;
    let result = scan_catalog_generation(&mut request, &catalog_path).and_then(|completed| {
        if !completed || request.cancellation.is_cancelled() {
            return Ok(None);
        }
        let source = Arc::new(CatalogShareSource::open_with_policy(
            &catalog_path,
            &request.storage.preview_cache_root,
            &request.storage.state_root,
            &request.display_name,
            request.private_provider_available,
            request.policy.clone(),
        )?);
        let photo_count = visible_photo_count(&source)?;
        write_generation_complete_marker(&generation_root, photo_count)?;
        write_published_catalog_pointer(&request.storage, &generation_id, photo_count)?;
        request.published_source.publish(source);
        Ok(Some(photo_count))
    });
    if !matches!(result, Ok(Some(_))) {
        let _ = remove_directory_if_present(&generation_root);
    }
    result
}

fn scan_catalog_generation(request: &mut IndexWorkerRequest, catalog_path: &Path) -> Result<bool> {
    let actor = open_catalog_actor_at(catalog_path)?;
    let mut catalog = actor.handle();
    let inspector = DecodeInspectionActor::spawn_with_cache(
        catalog.clone(),
        request
            .preview_runtime
            .take()
            .expect("index worker owns one preview runtime")
            .into_inspector(),
        &request.storage.preview_cache_root,
    )?;
    let inspection_handle = inspector.handle();
    let mut files_before_root = 0_u64;
    let mut completed = true;
    let scan_result = request.share_roots.iter().try_for_each(|root| {
        let progress = Arc::clone(&request.progress);
        let root_base = files_before_root;
        let report = scan_folder_with_inspection_controlled(
            &mut catalog,
            &inspection_handle,
            root,
            &request.cancellation,
            |scan| {
                let inspection = inspection_handle.progress_snapshot();
                let mut state = lock_progress(&progress);
                state.discovered_files = root_base.saturating_add(scan.files_seen);
                state.inspections_completed = inspection.summary.completed;
                state.published_previews = inspection.visual_artifacts_published;
            },
        )
        .with_context(|| format!("scan shared folder {}", root.display()))?;
        files_before_root = files_before_root.saturating_add(report.files_seen);
        if report.completion == ScanCompletion::Cancelled {
            completed = false;
        }
        Ok::<(), anyhow::Error>(())
    });
    let shutdown_result = inspector.shutdown();
    let inspection = inspection_handle.progress_snapshot();
    {
        let mut state = lock_progress(&request.progress);
        state.discovered_files = files_before_root;
        state.inspections_completed = inspection.summary.completed;
        state.published_previews = inspection.visual_artifacts_published;
    }
    drop(catalog);
    let actor_shutdown_result = actor.shutdown();
    scan_result?;
    shutdown_result?;
    actor_shutdown_result?;
    Ok(completed)
}

fn open_catalog_actor_at(path: &Path) -> Result<CatalogActor> {
    ensure_parent(path)?;
    match CatalogActor::spawn(path) {
        Ok(actor) => Ok(actor),
        Err(CatalogError::DevelopmentCatalogResetRequired { .. }) => {
            for catalog_path in catalog_files(path) {
                remove_file_if_present(&catalog_path)?;
            }
            CatalogActor::spawn(path).with_context(|| {
                format!(
                    "rebuild incompatible remote Library server Catalog {}",
                    path.display()
                )
            })
        }
        Err(error) => Err(error)
            .with_context(|| format!("open remote Library server Catalog {}", path.display())),
    }
}

impl Drop for LibraryServerService {
    fn drop(&mut self) {
        let Ok(state) = self.state.get_mut() else {
            return;
        };
        if let Some(mut running) = state.running.take() {
            running.index_job.cancellation.cancel();
            let _ = running.server.shutdown();
            let _ = join_index_worker(&mut running.index_job);
        }
    }
}

fn lock_progress(progress: &Mutex<IndexProgress>) -> std::sync::MutexGuard<'_, IndexProgress> {
    progress
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn join_finished_index_worker(job: &mut IndexJob) {
    if job.worker.as_ref().is_some_and(JoinHandle::is_finished) {
        let _ = join_index_worker(job);
    }
}

fn join_index_worker(job: &mut IndexJob) -> Result<()> {
    let Some(worker) = job.worker.take() else {
        return Ok(());
    };
    if worker.join().is_err() {
        let mut progress = lock_progress(&job.progress);
        progress.state = IndexState::Failed;
        progress.diagnostic = "remote Library background index worker panicked".to_owned();
        bail!("remote Library background index worker panicked");
    }
    Ok(())
}

fn catalog_generations_root(storage: &LibraryServerStorage) -> PathBuf {
    storage.state_root.join("catalog-generations")
}

fn published_catalog_pointer(storage: &LibraryServerStorage) -> PathBuf {
    storage.state_root.join("published-catalog")
}

struct PublishedCatalogPointer {
    path: PathBuf,
    photo_count: Option<u64>,
}

fn read_published_catalog(
    storage: &LibraryServerStorage,
) -> Result<Option<PublishedCatalogPointer>> {
    let pointer = published_catalog_pointer(storage);
    let value = match fs::read_to_string(&pointer) {
        Ok(value) => value,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error).with_context(|| format!("read {}", pointer.display())),
    };
    let mut lines = value.lines();
    let generation_id = lines.next().unwrap_or_default().trim().to_owned();
    Uuid::parse_str(&generation_id).with_context(|| {
        format!(
            "validate remote Library published Catalog generation {}",
            pointer.display()
        )
    })?;
    let catalog_path = catalog_generations_root(storage)
        .join(generation_id)
        .join("catalog.sqlite");
    if !catalog_path.is_file() {
        bail!(
            "published remote Library Catalog is missing: {}",
            catalog_path.display()
        );
    }
    let photo_count = lines
        .next()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::parse::<u64>)
        .transpose()
        .with_context(|| format!("parse published photo count from {}", pointer.display()))?;
    Ok(Some(PublishedCatalogPointer {
        path: catalog_path,
        photo_count,
    }))
}

fn write_published_catalog_pointer(
    storage: &LibraryServerStorage,
    generation_id: &str,
    photo_count: u64,
) -> Result<()> {
    let pointer = published_catalog_pointer(storage);
    let temporary = storage
        .state_root
        .join(format!("published-catalog-{}.tmp", Uuid::now_v7()));
    fs::write(&temporary, format!("{generation_id}\n{photo_count}\n"))
        .with_context(|| format!("write {}", temporary.display()))?;
    fs::rename(&temporary, &pointer).with_context(|| {
        format!(
            "publish remote Library Catalog generation at {}",
            pointer.display()
        )
    })
}

fn write_generation_complete_marker(generation_root: &Path, photo_count: u64) -> Result<()> {
    let marker = generation_root.join("complete");
    fs::write(&marker, format!("{photo_count}\n"))
        .with_context(|| format!("write {}", marker.display()))
}

fn prune_catalog_generations(storage: &LibraryServerStorage) -> Result<()> {
    let generations_root = catalog_generations_root(storage);
    let entries = match fs::read_dir(&generations_root) {
        Ok(entries) => entries,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => {
            return Err(error).with_context(|| format!("read {}", generations_root.display()));
        }
    };
    let published_id = read_published_catalog(storage)?.and_then(|published| {
        published
            .path
            .parent()
            .and_then(Path::file_name)
            .map(ToOwned::to_owned)
    });
    let mut candidates = Vec::new();
    for entry in entries {
        let entry =
            entry.with_context(|| format!("read entry in {}", generations_root.display()))?;
        if !entry
            .file_type()
            .with_context(|| format!("inspect {}", entry.path().display()))?
            .is_dir()
        {
            continue;
        }
        let file_name = entry.file_name();
        if Uuid::parse_str(&file_name.to_string_lossy()).is_err()
            || published_id.as_ref() == Some(&file_name)
        {
            continue;
        }
        if entry.path().join("complete").is_file() {
            candidates.push((file_name, entry.path()));
        } else {
            remove_directory_if_present(&entry.path())?;
        }
    }
    candidates.sort_by(|left, right| right.0.cmp(&left.0));
    for (_, path) in candidates.into_iter().skip(1) {
        remove_directory_if_present(&path)?;
    }
    Ok(())
}

fn visible_photo_count(source: &CatalogShareSource) -> Result<u64> {
    let mut cursor = None;
    let mut count = 0_u64;
    loop {
        let page = source
            .list_photos(cursor.as_deref(), MAX_LIBRARY_PAGE_SIZE)
            .map_err(|error| anyhow!("count shared Library manifest: {error:?}"))?;
        count = count.saturating_add(u64::try_from(page.items.len()).unwrap_or(u64::MAX));
        let Some(next_cursor) = page.next_cursor else {
            return Ok(count);
        };
        cursor = Some(next_cursor);
    }
}

fn normalized_share_roots(roots: Vec<PathBuf>) -> Result<Vec<PathBuf>> {
    if roots.is_empty() {
        bail!("add at least one folder before starting the remote Library server");
    }
    let mut seen = HashSet::new();
    let mut normalized = Vec::with_capacity(roots.len());
    for root in roots {
        let canonical = root
            .canonicalize()
            .with_context(|| format!("open shared folder {}", root.display()))?;
        if !canonical.is_dir() {
            bail!(
                "shared Library root is not a directory: {}",
                canonical.display()
            );
        }
        if seen.insert(canonical.clone()) {
            normalized.push(canonical);
        }
    }
    Ok(normalized)
}

fn ensure_parent(path: &Path) -> Result<()> {
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .ok_or_else(|| anyhow!("remote Library Catalog has no parent directory"))?;
    fs::create_dir_all(parent)
        .with_context(|| format!("create remote Library root {}", parent.display()))
}

fn directory_byte_len(root: &Path) -> Result<u64> {
    let entries = match fs::read_dir(root) {
        Ok(entries) => entries,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(0),
        Err(error) => return Err(error).with_context(|| format!("read {}", root.display())),
    };
    let mut total = 0_u64;
    for entry in entries {
        let entry = entry.with_context(|| format!("read entry in {}", root.display()))?;
        let file_type = entry
            .file_type()
            .with_context(|| format!("inspect {}", entry.path().display()))?;
        if file_type.is_symlink() {
            continue;
        }
        total = total.saturating_add(if file_type.is_dir() {
            directory_byte_len(&entry.path())?
        } else if file_type.is_file() {
            entry
                .metadata()
                .with_context(|| format!("inspect {}", entry.path().display()))?
                .len()
        } else {
            0
        });
    }
    Ok(total)
}

fn catalog_files(path: &Path) -> [PathBuf; 3] {
    [
        path.to_path_buf(),
        PathBuf::from(format!("{}-wal", path.display())),
        PathBuf::from(format!("{}-shm", path.display())),
    ]
}

fn remove_file_if_present(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error).with_context(|| format!("remove {}", path.display())),
    }
}

fn remove_directory_if_present(path: &Path) -> Result<()> {
    match fs::remove_dir_all(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error).with_context(|| format!("remove {}", path.display())),
    }
}

#[cfg(test)]
mod tests;
