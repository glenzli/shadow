use std::{
    io,
    net::{SocketAddr, TcpStream},
    time::Duration,
};

use shadow_domain::{PhotoId, RepresentationId};
use thiserror::Error;

use crate::{
    protocol::{
        LIBRARY_PROTOCOL_REVISION, MAX_LIBRARY_PAGE_SIZE, MAX_ORIGINAL_CHUNK_BYTES, OriginalChunk,
        PreparedOriginal, RemoteErrorCode, RemotePhotoPage, RemotePreviewManifest, Request,
        RequestEnvelope, ResponseValue, ServerInfo,
    },
    server::AuthorizationToken,
    transport::{TransportError, read_response, write_request},
};

#[derive(Debug, Clone)]
pub struct LibraryClientConfig {
    pub server_address: SocketAddr,
    pub authorization: AuthorizationToken,
    pub connect_timeout: Duration,
    pub read_timeout: Duration,
    pub write_timeout: Duration,
}

impl LibraryClientConfig {
    #[must_use]
    pub fn new(server_address: SocketAddr, authorization: AuthorizationToken) -> Self {
        Self {
            server_address,
            authorization,
            connect_timeout: Duration::from_secs(10),
            read_timeout: Duration::from_secs(30),
            write_timeout: Duration::from_secs(30),
        }
    }
}

#[derive(Debug, Clone)]
pub struct LibraryClient {
    config: LibraryClientConfig,
}

impl LibraryClient {
    #[must_use]
    pub fn new(config: LibraryClientConfig) -> Self {
        Self { config }
    }

    /// Reads the server identity and capability contract.
    ///
    /// # Errors
    ///
    /// Returns a transport, authentication, version, or response-identity error.
    pub fn server_info(&self) -> Result<ServerInfo, LibraryClientError> {
        let (value, body) = self.request(Request::ServerInfo)?;
        require_empty_body(&body)?;
        match value {
            ResponseValue::ServerInfo(info) => Ok(info),
            other => Err(unexpected("server_info", &other)),
        }
    }

    /// Reads one bounded page of the remote manifest.
    ///
    /// # Errors
    ///
    /// Returns a transport or remote protocol error, including an expired cursor.
    pub fn list_photos(
        &self,
        cursor: Option<String>,
        limit: u16,
    ) -> Result<RemotePhotoPage, LibraryClientError> {
        let (value, body) = self.request(Request::ListPhotos {
            cursor,
            limit: limit.clamp(1, MAX_LIBRARY_PAGE_SIZE),
        })?;
        require_empty_body(&body)?;
        match value {
            ResponseValue::PhotoPage(page) => Ok(page),
            other => Err(unexpected("photo_page", &other)),
        }
    }

    /// Downloads and verifies one content-addressed preview admitted by a manifest page.
    ///
    /// # Errors
    ///
    /// Returns a transport error or an identity error when length or BLAKE3 differs.
    pub fn fetch_preview(
        &self,
        digest_blake3: [u8; 32],
    ) -> Result<(RemotePreviewManifest, Vec<u8>), LibraryClientError> {
        let (value, body) = self.request(Request::FetchPreview { digest_blake3 })?;
        let manifest = match value {
            ResponseValue::Preview(manifest) => manifest,
            other => return Err(unexpected("preview", &other)),
        };
        if manifest.digest_blake3 != digest_blake3 {
            return Err(LibraryClientError::IdentityMismatch(
                "preview digest differs from the requested digest".to_owned(),
            ));
        }
        let actual_len = u64::try_from(body.len()).unwrap_or(u64::MAX);
        if actual_len != manifest.byte_len {
            return Err(LibraryClientError::IdentityMismatch(format!(
                "preview body has {actual_len} bytes, expected {}",
                manifest.byte_len
            )));
        }
        if blake3::hash(&body).as_bytes() != &manifest.digest_blake3 {
            return Err(LibraryClientError::IdentityMismatch(
                "preview body failed BLAKE3 verification".to_owned(),
            ));
        }
        Ok((manifest, body))
    }

    /// Freezes one exact original revision before chunk transfer.
    ///
    /// # Errors
    ///
    /// Returns an error when the source is absent, stale, unauthorized, or unavailable.
    pub fn prepare_original(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<PreparedOriginal, LibraryClientError> {
        let (value, body) = self.request(Request::PrepareOriginal {
            photo_id,
            representation_id,
        })?;
        require_empty_body(&body)?;
        match value {
            ResponseValue::PreparedOriginal(manifest) => {
                if manifest.photo_id != photo_id || manifest.representation_id != representation_id
                {
                    return Err(LibraryClientError::IdentityMismatch(
                        "prepared original identity differs from the request".to_owned(),
                    ));
                }
                Ok(manifest)
            }
            other => Err(unexpected("prepared_original", &other)),
        }
    }

    /// Reads one bounded chunk from a prepared original revision.
    ///
    /// # Errors
    ///
    /// Returns a transport, lease, range, or response-identity error.
    pub fn read_original(
        &self,
        revision_token: String,
        offset: u64,
        maximum_bytes: u32,
    ) -> Result<(OriginalChunk, Vec<u8>), LibraryClientError> {
        let (value, body) = self.request(Request::ReadOriginal {
            revision_token,
            offset,
            maximum_bytes: maximum_bytes.clamp(1, MAX_ORIGINAL_CHUNK_BYTES),
        })?;
        let chunk = match value {
            ResponseValue::OriginalChunk(chunk) => chunk,
            other => return Err(unexpected("original_chunk", &other)),
        };
        if chunk.offset != offset {
            return Err(LibraryClientError::IdentityMismatch(format!(
                "original chunk starts at {}, expected {offset}",
                chunk.offset
            )));
        }
        if body.len() > usize::try_from(maximum_bytes).unwrap_or(usize::MAX) {
            return Err(LibraryClientError::IdentityMismatch(
                "original chunk exceeds the requested bound".to_owned(),
            ));
        }
        Ok((chunk, body))
    }

    fn request(&self, request: Request) -> Result<(ResponseValue, Vec<u8>), LibraryClientError> {
        let mut stream =
            TcpStream::connect_timeout(&self.config.server_address, self.config.connect_timeout)?;
        stream.set_read_timeout(Some(self.config.read_timeout))?;
        stream.set_write_timeout(Some(self.config.write_timeout))?;
        write_request(
            &mut stream,
            &RequestEnvelope {
                protocol_revision: LIBRARY_PROTOCOL_REVISION,
                authorization: self.config.authorization.as_str().to_owned(),
                request,
            },
        )?;
        let (header, body) = read_response(&mut stream)?;
        if header.protocol_revision != LIBRARY_PROTOCOL_REVISION {
            return Err(LibraryClientError::ProtocolRevision {
                actual: header.protocol_revision,
                expected: LIBRARY_PROTOCOL_REVISION,
            });
        }
        let value = header.value.map_err(|error| LibraryClientError::Remote {
            code: error.code,
            message: error.message,
        })?;
        Ok((value, body))
    }
}

#[derive(Debug, Error)]
pub enum LibraryClientError {
    #[error("connect to remote Library failed: {0}")]
    Io(#[from] io::Error),
    #[error("remote Library transport failed: {0}")]
    Transport(#[from] TransportError),
    #[error("remote Library rejected the request ({code:?}): {message}")]
    Remote {
        code: RemoteErrorCode,
        message: String,
    },
    #[error("remote Library protocol revision is {actual}, expected {expected}")]
    ProtocolRevision { actual: u32, expected: u32 },
    #[error("remote Library returned an unexpected {actual} response for {expected}")]
    UnexpectedResponse {
        expected: &'static str,
        actual: &'static str,
    },
    #[error("remote Library content identity mismatch: {0}")]
    IdentityMismatch(String),
}

fn require_empty_body(body: &[u8]) -> Result<(), LibraryClientError> {
    if body.is_empty() {
        Ok(())
    } else {
        Err(LibraryClientError::IdentityMismatch(
            "metadata response unexpectedly carried a binary body".to_owned(),
        ))
    }
}

fn unexpected(expected: &'static str, actual: &ResponseValue) -> LibraryClientError {
    LibraryClientError::UnexpectedResponse {
        expected,
        actual: response_name(actual),
    }
}

const fn response_name(value: &ResponseValue) -> &'static str {
    match value {
        ResponseValue::ServerInfo(_) => "server_info",
        ResponseValue::PhotoPage(_) => "photo_page",
        ResponseValue::Preview(_) => "preview",
        ResponseValue::PreparedOriginal(_) => "prepared_original",
        ResponseValue::OriginalChunk(_) => "original_chunk",
    }
}

#[cfg(test)]
mod tests;
