use std::{
    io,
    net::{SocketAddr, TcpListener, TcpStream},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

use thiserror::Error;

use crate::{
    protocol::{
        LIBRARY_PROTOCOL_REVISION, MAX_LIBRARY_PAGE_SIZE, MAX_ORIGINAL_CHUNK_BYTES, OriginalChunk,
        PreparedOriginal, RemoteError, RemoteErrorCode, RemotePhotoPage, RemotePreviewManifest,
        Request, ResponseHeader, ResponseValue, ServerInfo,
    },
    transport::{TransportError, read_request, write_response},
};

const MINIMUM_TOKEN_BYTES: usize = 32;
const MAXIMUM_TOKEN_BYTES: usize = 512;
const MAXIMUM_ACTIVE_CONNECTIONS: usize = 16;

#[derive(Clone)]
pub struct AuthorizationToken(Arc<str>);

impl std::fmt::Debug for AuthorizationToken {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_tuple("AuthorizationToken")
            .field(&"<redacted>")
            .finish()
    }
}

impl AuthorizationToken {
    /// Validates a pre-shared token without retaining any printable debug representation.
    ///
    /// # Errors
    ///
    /// Returns an error when the token is too short/long or contains control characters.
    pub fn parse(value: impl Into<String>) -> Result<Self, AuthorizationTokenError> {
        let value = value.into();
        let byte_len = value.len();
        if !(MINIMUM_TOKEN_BYTES..=MAXIMUM_TOKEN_BYTES).contains(&byte_len) {
            return Err(AuthorizationTokenError::Length {
                actual: byte_len,
                minimum: MINIMUM_TOKEN_BYTES,
                maximum: MAXIMUM_TOKEN_BYTES,
            });
        }
        if value.chars().any(char::is_control) {
            return Err(AuthorizationTokenError::ControlCharacter);
        }
        Ok(Self(Arc::from(value)))
    }

    pub(crate) fn as_str(&self) -> &str {
        &self.0
    }

    fn authorizes(&self, candidate: &str) -> bool {
        constant_time_equal(self.0.as_bytes(), candidate.as_bytes())
    }
}

#[derive(Debug, Error)]
pub enum AuthorizationTokenError {
    #[error("sharing token has {actual} bytes; expected {minimum}..={maximum}")]
    Length {
        actual: usize,
        minimum: usize,
        maximum: usize,
    },
    #[error("sharing token must not contain control characters")]
    ControlCharacter,
}

pub trait LibraryShareSource: Send + Sync + 'static {
    fn server_info(&self) -> ServerInfo;
    /// Reads one opaque-cursor manifest page.
    ///
    /// # Errors
    ///
    /// Returns a bounded wire error when the cursor or backing source is unavailable.
    fn list_photos(&self, cursor: Option<&str>, limit: u16)
    -> Result<RemotePhotoPage, RemoteError>;
    /// Reads one manifest-admitted preview.
    ///
    /// # Errors
    ///
    /// Returns a bounded wire error when identity, cache, or authorization state is invalid.
    fn fetch_preview(
        &self,
        digest_blake3: [u8; 32],
    ) -> Result<(RemotePreviewManifest, Vec<u8>), RemoteError>;
    /// Freezes and hashes one exact original source revision.
    ///
    /// # Errors
    ///
    /// Returns a bounded wire error when the source is unknown, stale, or unreadable.
    fn prepare_original(
        &self,
        photo_id: shadow_domain::PhotoId,
        representation_id: shadow_domain::RepresentationId,
    ) -> Result<PreparedOriginal, RemoteError>;
    /// Reads one bounded chunk from a prepared original lease.
    ///
    /// # Errors
    ///
    /// Returns a bounded wire error for an expired lease, invalid range, or changed source.
    fn read_original(
        &self,
        revision_token: &str,
        offset: u64,
        maximum_bytes: u32,
    ) -> Result<(OriginalChunk, Vec<u8>), RemoteError>;
}

#[derive(Debug, Clone)]
pub struct LibraryServerConfig {
    pub bind_address: SocketAddr,
    pub authorization: AuthorizationToken,
    pub read_timeout: Duration,
    pub write_timeout: Duration,
}

impl LibraryServerConfig {
    #[must_use]
    pub fn new(bind_address: SocketAddr, authorization: AuthorizationToken) -> Self {
        Self {
            bind_address,
            authorization,
            read_timeout: Duration::from_secs(30),
            write_timeout: Duration::from_secs(30),
        }
    }
}

pub struct LibraryServer {
    config: LibraryServerConfig,
    source: Arc<dyn LibraryShareSource>,
}

impl std::fmt::Debug for LibraryServer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibraryServer")
            .field("config", &self.config)
            .field("source", &"<LibraryShareSource>")
            .finish()
    }
}

impl LibraryServer {
    #[must_use]
    pub fn new(config: LibraryServerConfig, source: Arc<dyn LibraryShareSource>) -> Self {
        Self { config, source }
    }

    /// Binds the configured listener and starts authenticated request admission.
    ///
    /// # Errors
    ///
    /// Returns an I/O error when the listener or worker cannot start.
    pub fn start(self) -> Result<RunningLibraryServer, LibraryServerError> {
        let listener = TcpListener::bind(self.config.bind_address)?;
        listener.set_nonblocking(true)?;
        let local_address = listener.local_addr()?;
        let shutdown = Arc::new(AtomicBool::new(false));
        let active = Arc::new(AtomicUsize::new(0));
        let worker_shutdown = Arc::clone(&shutdown);
        let worker_active = Arc::clone(&active);
        let join = thread::Builder::new()
            .name("shadow-library-server".to_owned())
            .spawn(move || {
                run_listener(
                    listener,
                    self.config,
                    self.source,
                    worker_shutdown,
                    worker_active,
                )
            })?;
        Ok(RunningLibraryServer {
            local_address,
            shutdown,
            join: Some(join),
        })
    }
}

#[derive(Debug)]
pub struct RunningLibraryServer {
    local_address: SocketAddr,
    shutdown: Arc<AtomicBool>,
    join: Option<JoinHandle<Result<(), LibraryServerError>>>,
}

impl RunningLibraryServer {
    pub const fn local_address(&self) -> SocketAddr {
        self.local_address
    }

    /// Stops admission and waits until every admitted connection has completed.
    ///
    /// # Errors
    ///
    /// Returns an error when the worker panicked, failed, or was already joined.
    pub fn shutdown(mut self) -> Result<(), LibraryServerError> {
        self.shutdown.store(true, Ordering::Release);
        self.join
            .take()
            .ok_or(LibraryServerError::AlreadyStopped)?
            .join()
            .map_err(|_| LibraryServerError::WorkerPanicked)??;
        Ok(())
    }
}

impl Drop for RunningLibraryServer {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::Release);
    }
}

#[derive(Debug, Error)]
pub enum LibraryServerError {
    #[error("remote Library server I/O failed: {0}")]
    Io(#[from] io::Error),
    #[error("remote Library server transport failed: {0}")]
    Transport(#[from] TransportError),
    #[error("remote Library server is already stopped")]
    AlreadyStopped,
    #[error("remote Library server worker panicked")]
    WorkerPanicked,
}

// These values are deliberately moved into the dedicated listener thread; borrowing would tie
// their lifetime to `LibraryServer::start` and make the running handle unsound.
#[allow(clippy::needless_pass_by_value)]
fn run_listener(
    listener: TcpListener,
    config: LibraryServerConfig,
    source: Arc<dyn LibraryShareSource>,
    shutdown: Arc<AtomicBool>,
    active: Arc<AtomicUsize>,
) -> Result<(), LibraryServerError> {
    while !shutdown.load(Ordering::Acquire) {
        match listener.accept() {
            Ok((stream, _)) => {
                if active.fetch_add(1, Ordering::AcqRel) >= MAXIMUM_ACTIVE_CONNECTIONS {
                    active.fetch_sub(1, Ordering::AcqRel);
                    continue;
                }
                let source = Arc::clone(&source);
                let authorization = config.authorization.clone();
                let active = Arc::clone(&active);
                let read_timeout = config.read_timeout;
                let write_timeout = config.write_timeout;
                thread::spawn(move || {
                    let _guard = ActiveConnectionGuard(active);
                    let _ = handle_connection(
                        stream,
                        &authorization,
                        source.as_ref(),
                        read_timeout,
                        write_timeout,
                    );
                });
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(10));
            }
            Err(error) => return Err(error.into()),
        }
    }
    while active.load(Ordering::Acquire) != 0 {
        thread::sleep(Duration::from_millis(5));
    }
    Ok(())
}

struct ActiveConnectionGuard(Arc<AtomicUsize>);

impl Drop for ActiveConnectionGuard {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

fn handle_connection(
    mut stream: TcpStream,
    authorization: &AuthorizationToken,
    source: &dyn LibraryShareSource,
    read_timeout: Duration,
    write_timeout: Duration,
) -> Result<(), LibraryServerError> {
    stream.set_read_timeout(Some(read_timeout))?;
    stream.set_write_timeout(Some(write_timeout))?;
    let envelope = read_request(&mut stream)?;
    let (value, body) = if !authorization.authorizes(&envelope.authorization) {
        (
            Err(remote_error(
                RemoteErrorCode::Unauthorized,
                "authorization failed",
            )),
            Vec::new(),
        )
    } else if envelope.protocol_revision != LIBRARY_PROTOCOL_REVISION {
        (
            Err(remote_error(
                RemoteErrorCode::InvalidRequest,
                "unsupported Library protocol revision",
            )),
            Vec::new(),
        )
    } else {
        dispatch(source, envelope.request)
    };
    let header = ResponseHeader {
        protocol_revision: LIBRARY_PROTOCOL_REVISION,
        body_byte_len: u64::try_from(body.len()).unwrap_or(u64::MAX),
        value,
    };
    write_response(&mut stream, &header, &body)?;
    Ok(())
}

fn dispatch(
    source: &dyn LibraryShareSource,
    request: Request,
) -> (Result<ResponseValue, RemoteError>, Vec<u8>) {
    let result = match request {
        Request::ServerInfo => Ok((ResponseValue::ServerInfo(source.server_info()), Vec::new())),
        Request::ListPhotos { cursor, limit } => source
            .list_photos(cursor.as_deref(), limit.clamp(1, MAX_LIBRARY_PAGE_SIZE))
            .map(|page| (ResponseValue::PhotoPage(page), Vec::new())),
        Request::FetchPreview { digest_blake3 } => source
            .fetch_preview(digest_blake3)
            .map(|(manifest, body)| (ResponseValue::Preview(manifest), body)),
        Request::PrepareOriginal {
            photo_id,
            representation_id,
        } => source
            .prepare_original(photo_id, representation_id)
            .map(|manifest| (ResponseValue::PreparedOriginal(manifest), Vec::new())),
        Request::ReadOriginal {
            revision_token,
            offset,
            maximum_bytes,
        } => source
            .read_original(
                &revision_token,
                offset,
                maximum_bytes.clamp(1, MAX_ORIGINAL_CHUNK_BYTES),
            )
            .map(|(chunk, body)| (ResponseValue::OriginalChunk(chunk), body)),
    };
    match result {
        Ok((value, body)) => (Ok(value), body),
        Err(error) => (Err(error), Vec::new()),
    }
}

pub(crate) fn remote_error(code: RemoteErrorCode, message: impl Into<String>) -> RemoteError {
    RemoteError {
        code,
        message: message.into(),
    }
}

fn constant_time_equal(left: &[u8], right: &[u8]) -> bool {
    let mut difference = left.len() ^ right.len();
    let maximum = left.len().max(right.len());
    for index in 0..maximum {
        let left_byte = left.get(index).copied().unwrap_or_default();
        let right_byte = right.get(index).copied().unwrap_or_default();
        difference |= usize::from(left_byte ^ right_byte);
    }
    difference == 0
}

#[cfg(test)]
mod tests;
