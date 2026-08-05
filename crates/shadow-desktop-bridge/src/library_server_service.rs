//! Managed remote-Library server lifecycle for desktop and operator clients.
//!
//! This owner keeps provider discovery, shared-root scanning, authenticated listener lifetime,
//! cache inventory, and reset policy together. UI and CLI facades supply configuration but never
//! own a server thread or private decoder lifecycle.

mod provider_host;

use std::{
    collections::HashSet,
    fs,
    net::SocketAddr,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use anyhow::{Context, Result, anyhow, bail};
use shadow_catalog::{CatalogActor, CatalogError};
use shadow_core::{DecodeInspectionActor, scan_folder_with_inspection};
use shadow_library_sharing::{
    AuthorizationToken, CatalogSharePolicy, CatalogShareSource, LibraryServer, LibraryServerConfig,
    LibraryShareSource, RunningLibraryServer, protocol::MAX_LIBRARY_PAGE_SIZE,
};

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
}

#[derive(Debug)]
struct RunningState {
    server: RunningLibraryServer,
    display_name: String,
    provider_mode: String,
    photo_count: u64,
    shared_root_count: u64,
    serves_originals: bool,
}

#[derive(Debug, Default)]
struct ServiceState {
    running: Option<RunningState>,
    last_snapshot: Option<LibraryServerSnapshot>,
}

#[derive(Debug)]
pub struct LibraryServerService {
    storage: LibraryServerStorage,
    state: Mutex<ServiceState>,
}

impl LibraryServerService {
    #[must_use]
    pub fn new(storage: LibraryServerStorage) -> Self {
        Self {
            storage,
            state: Mutex::new(ServiceState::default()),
        }
    }

    /// Scans all configured roots and starts one authenticated listener.
    ///
    /// # Errors
    ///
    /// Fails for invalid roots/configuration, provider preparation failures, Catalog/cache errors,
    /// or listener admission failures. A failed start never leaves a running listener behind.
    pub fn start(&self, request: LibraryServerStartRequest) -> Result<LibraryServerSnapshot> {
        let mut state = self.lock_state()?;
        if state.running.is_some() {
            bail!("remote Library server is already running");
        }
        let display_name = request.display_name.trim().to_owned();
        if display_name.is_empty() {
            bail!("remote Library server name must not be empty");
        }
        let share_roots = normalized_share_roots(request.share_roots)?;
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
        self.scan_roots(&share_roots, preview_runtime)?;
        let source = CatalogShareSource::open_with_policy(
            &self.storage.catalog_path,
            &self.storage.preview_cache_root,
            &self.storage.state_root,
            &display_name,
            private_provider_available,
            CatalogSharePolicy::for_roots(share_roots.clone(), request.serves_originals),
        )?;
        let photo_count = visible_photo_count(&source)?;
        let server = LibraryServer::new(
            LibraryServerConfig::new(request.bind_address, request.authorization),
            Arc::new(source),
        )
        .start()?;
        let snapshot = LibraryServerSnapshot {
            running: true,
            local_address: Some(server.local_address()),
            display_name: display_name.clone(),
            provider_mode: provider_mode.clone(),
            photo_count,
            cache_byte_len: directory_byte_len(&self.storage.preview_cache_root)?,
            shared_root_count: u64::try_from(share_roots.len()).unwrap_or(u64::MAX),
            serves_originals: request.serves_originals,
        };
        state.running = Some(RunningState {
            server,
            display_name,
            provider_mode,
            photo_count,
            shared_root_count: snapshot.shared_root_count,
            serves_originals: request.serves_originals,
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
        let running = self
            .lock_state()?
            .running
            .take()
            .ok_or_else(|| anyhow!("remote Library server is not running"))?;
        let RunningState {
            server,
            display_name,
            provider_mode,
            photo_count,
            shared_root_count,
            serves_originals,
        } = running;
        server.shutdown()?;
        let snapshot = LibraryServerSnapshot {
            running: false,
            local_address: None,
            display_name,
            provider_mode,
            photo_count,
            cache_byte_len: directory_byte_len(&self.storage.preview_cache_root)?,
            shared_root_count,
            serves_originals,
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
        let state = self.lock_state()?;
        if let Some(running) = &state.running {
            return Ok(LibraryServerSnapshot {
                running: true,
                local_address: Some(running.server.local_address()),
                display_name: running.display_name.clone(),
                provider_mode: running.provider_mode.clone(),
                photo_count: running.photo_count,
                cache_byte_len: directory_byte_len(&self.storage.preview_cache_root)?,
                shared_root_count: running.shared_root_count,
                serves_originals: running.serves_originals,
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
        if state.running.is_some() {
            bail!("stop the remote Library server before clearing its cache");
        }
        for catalog_path in catalog_files(&self.storage.catalog_path) {
            remove_file_if_present(&catalog_path)?;
        }
        remove_directory_if_present(&self.storage.preview_cache_root)?;
        state.last_snapshot = None;
        drop(state);
        self.snapshot()
    }

    fn scan_roots(
        &self,
        share_roots: &[PathBuf],
        preview_runtime: LibraryServerPreviewRuntime,
    ) -> Result<()> {
        let actor = self.open_catalog_actor()?;
        let mut catalog = actor.handle();
        let inspector = DecodeInspectionActor::spawn_with_cache(
            catalog.clone(),
            preview_runtime.into_inspector(),
            &self.storage.preview_cache_root,
        )?;
        let scan_result = share_roots.iter().try_for_each(|root| {
            scan_folder_with_inspection(&mut catalog, &inspector.handle(), root)
                .with_context(|| format!("scan shared folder {}", root.display()))
                .map(|_| ())
        });
        let shutdown_result = inspector.shutdown();
        scan_result?;
        shutdown_result?;
        drop(catalog);
        actor.shutdown()?;
        Ok(())
    }

    fn open_catalog_actor(&self) -> Result<CatalogActor> {
        match CatalogActor::spawn(&self.storage.catalog_path) {
            Ok(actor) => Ok(actor),
            Err(CatalogError::DevelopmentCatalogResetRequired { .. }) => {
                for catalog_path in catalog_files(&self.storage.catalog_path) {
                    remove_file_if_present(&catalog_path)?;
                }
                CatalogActor::spawn(&self.storage.catalog_path).with_context(|| {
                    format!(
                        "rebuild incompatible remote Library server Catalog {}",
                        self.storage.catalog_path.display()
                    )
                })
            }
            Err(error) => Err(error).with_context(|| {
                format!(
                    "open remote Library server Catalog {}",
                    self.storage.catalog_path.display()
                )
            }),
        }
    }

    fn lock_state(&self) -> Result<std::sync::MutexGuard<'_, ServiceState>> {
        self.state
            .lock()
            .map_err(|_| anyhow!("remote Library server state lock is poisoned"))
    }
}

impl Drop for LibraryServerService {
    fn drop(&mut self) {
        let Ok(state) = self.state.get_mut() else {
            return;
        };
        if let Some(running) = state.running.take() {
            let _ = running.server.shutdown();
        }
    }
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
