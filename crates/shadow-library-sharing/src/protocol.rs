//! Versioned values exchanged by a remote Library server and client.
//!
//! The protocol intentionally carries stable identities, bounded metadata,
//! and content digests. Native paths and decoder-private values never cross
//! this boundary.

use serde::{Deserialize, Serialize};
use shadow_domain::{ImageDimensions, PhotoId, PreviewCodec, RepresentationId, RepresentationKind};
use uuid::Uuid;

pub const LIBRARY_PROTOCOL_VERSION: u32 = 1;
pub const MAX_LIBRARY_PAGE_SIZE: u16 = 256;
pub const MAX_ORIGINAL_CHUNK_BYTES: u32 = 4 * 1_024 * 1_024;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ServerId(pub Uuid);

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ServerInfo {
    pub protocol_version: u32,
    pub server_id: ServerId,
    pub display_name: String,
    pub capabilities: ServerCapabilities,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ServerCapabilities {
    pub serves_embedded_previews: CapabilityAvailability,
    pub serves_generated_proxies: CapabilityAvailability,
    pub serves_originals: CapabilityAvailability,
    pub private_preview_provider: CapabilityAvailability,
    pub maximum_page_size: u16,
    pub maximum_original_chunk_bytes: u32,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CapabilityAvailability {
    Available,
    Unavailable,
}

impl CapabilityAvailability {
    pub const fn is_available(self) -> bool {
        matches!(self, Self::Available)
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RemotePhotoPage {
    pub server_id: ServerId,
    pub items: Vec<RemotePhotoManifest>,
    pub next_cursor: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RemotePhotoManifest {
    pub photo_id: PhotoId,
    /// RAW-preferred representation used for the browse preview and default
    /// edit admission.
    pub representation_id: RepresentationId,
    pub display_name: String,
    pub source_byte_len: u64,
    pub source_modified_at_ms: Option<i64>,
    pub metadata: RemotePhotoMetadata,
    pub preview: RemotePreviewAvailability,
    /// All currently known original representations of this logical photo.
    /// Older persisted mirrors deserialize an empty inventory and retain the
    /// legacy preferred-representation fields above.
    #[serde(default)]
    pub representations: Vec<RemoteRepresentationManifest>,
}

impl RemotePhotoManifest {
    #[must_use]
    pub fn preferred_original_digest(&self) -> Option<[u8; 32]> {
        self.representations
            .iter()
            .find(|representation| representation.representation_id == self.representation_id)
            .and_then(|representation| match representation.original_identity {
                RemoteOriginalIdentity::NotPrepared => None,
                RemoteOriginalIdentity::Available { digest_blake3 } => Some(digest_blake3),
            })
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RemoteRepresentationManifest {
    pub representation_id: RepresentationId,
    pub kind: RepresentationKind,
    pub display_name: String,
    pub source_byte_len: u64,
    pub source_modified_at_ms: Option<i64>,
    pub location_count: u32,
    pub online_location_count: u32,
    #[serde(default)]
    pub original_identity: RemoteOriginalIdentity,
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "availability", rename_all = "snake_case")]
pub enum RemoteOriginalIdentity {
    #[default]
    NotPrepared,
    Available {
        digest_blake3: [u8; 32],
    },
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct RemotePhotoMetadata {
    pub captured_at_unix_seconds: Option<i64>,
    pub camera_make: String,
    pub camera_model: String,
    pub lens_make: String,
    pub lens_model: String,
    pub iso_speed: Option<f64>,
    pub exposure_time_seconds: Option<f64>,
    pub aperture_f_number: Option<f64>,
    pub focal_length_mm: Option<f64>,
    pub raw_dimensions: Option<ImageDimensions>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(tag = "availability", rename_all = "snake_case")]
pub enum RemotePreviewAvailability {
    Available(RemotePreviewManifest),
    Unavailable { reason: PreviewUnavailableReason },
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RemotePreviewManifest {
    pub role: RemotePreviewRole,
    pub digest_blake3: [u8; 32],
    pub byte_len: u64,
    pub codec: PreviewCodec,
    pub dimensions: ImageDimensions,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemotePreviewRole {
    EmbeddedPreview,
    GeneratedProxy,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PreviewUnavailableReason {
    NotPrepared,
    DecoderCapabilityMissing,
    CacheUnavailable,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PreparedOriginal {
    pub server_id: ServerId,
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub revision_token: String,
    pub digest_blake3: [u8; 32],
    pub byte_len: u64,
    pub display_name: String,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct OriginalChunk {
    pub offset: u64,
    pub complete: bool,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(tag = "request", rename_all = "snake_case")]
pub(crate) enum Request {
    ServerInfo,
    ListPhotos {
        cursor: Option<String>,
        limit: u16,
    },
    FetchPreview {
        digest_blake3: [u8; 32],
    },
    PrepareOriginal {
        photo_id: PhotoId,
        representation_id: RepresentationId,
    },
    ReadOriginal {
        revision_token: String,
        offset: u64,
        maximum_bytes: u32,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "response", content = "value", rename_all = "snake_case")]
pub(crate) enum ResponseValue {
    ServerInfo(ServerInfo),
    PhotoPage(RemotePhotoPage),
    Preview(RemotePreviewManifest),
    PreparedOriginal(PreparedOriginal),
    OriginalChunk(OriginalChunk),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub(crate) struct ResponseHeader {
    pub protocol_version: u32,
    pub value: Result<ResponseValue, RemoteError>,
    pub body_byte_len: u64,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RemoteError {
    pub code: RemoteErrorCode,
    pub message: String,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteErrorCode {
    Unauthorized,
    InvalidRequest,
    NotFound,
    StaleSource,
    Unavailable,
    Internal,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub(crate) struct RequestEnvelope {
    pub protocol_version: u32,
    pub authorization: String,
    pub request: Request,
}

#[cfg(test)]
mod tests;
