//! Dated-revision values exchanged by a remote Library server and client.
//!
//! The protocol intentionally carries stable identities, bounded metadata,
//! and content digests. Native paths and decoder-private values never cross
//! this boundary.

use serde::{Deserialize, Serialize};
use shadow_domain::{
    GpsMetadataSnapshot, ImageDimensions, PhotoId, PreviewCodec, RawMetadataSnapshot,
    RepresentationId, RepresentationKind,
};
use uuid::Uuid;

/// Current wire revision encoded as YYYYMMDDNN, where NN is the contract's
/// daily sequence. Compatible additions use capabilities; incompatible wire
/// changes advance this revision.
pub const LIBRARY_PROTOCOL_REVISION: u32 = 2_026_080_601;
pub const MAX_LIBRARY_PAGE_SIZE: u16 = 256;
pub const MAX_ORIGINAL_CHUNK_BYTES: u32 = 4 * 1_024 * 1_024;
pub const REMOTE_PHOTO_METADATA_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ServerId(pub Uuid);

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ServerInfo {
    pub protocol_revision: u32,
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

/// Provider-neutral metadata suitable for Library presentation.
///
/// This is deliberately not an EXIF or MakerNote byte container. New fields
/// remain optional so a newer client can read an older persisted mirror and an
/// older peer can ignore compatible additions.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct RemotePhotoMetadata {
    pub schema_version: u32,
    pub captured_at_unix_seconds: Option<i64>,
    pub camera_make: String,
    pub camera_model: String,
    pub lens_make: String,
    pub lens_model: String,
    pub iso_speed: Option<f64>,
    pub exposure_time_seconds: Option<f64>,
    pub aperture_f_number: Option<f64>,
    pub focal_length_mm: Option<f64>,
    pub focal_length_35mm: Option<f64>,
    pub raw_dimensions: Option<ImageDimensions>,
    /// Display-oriented uncropped image dimensions reported by the decoder.
    pub image_dimensions: Option<ImageDimensions>,
    /// Provider orientation code retained for metadata presentation and
    /// source-aware consumers. Preview pixels use their own explicit contract.
    pub orientation: Option<i32>,
    pub gps: Option<GpsMetadataSnapshot>,
}

impl Default for RemotePhotoMetadata {
    fn default() -> Self {
        Self {
            schema_version: REMOTE_PHOTO_METADATA_SCHEMA_VERSION,
            captured_at_unix_seconds: None,
            camera_make: String::new(),
            camera_model: String::new(),
            lens_make: String::new(),
            lens_model: String::new(),
            iso_speed: None,
            exposure_time_seconds: None,
            aperture_f_number: None,
            focal_length_mm: None,
            focal_length_35mm: None,
            raw_dimensions: None,
            image_dimensions: None,
            orientation: None,
            gps: None,
        }
    }
}

impl From<&RawMetadataSnapshot> for RemotePhotoMetadata {
    fn from(metadata: &RawMetadataSnapshot) -> Self {
        Self {
            schema_version: REMOTE_PHOTO_METADATA_SCHEMA_VERSION,
            captured_at_unix_seconds: nonzero_i64(metadata.captured_at_unix_seconds),
            camera_make: metadata.make.clone(),
            camera_model: metadata.model.clone(),
            lens_make: metadata.lens_make.clone(),
            lens_model: metadata.lens_model.clone(),
            iso_speed: positive_f64(metadata.iso_speed),
            exposure_time_seconds: positive_f64(metadata.exposure_time_seconds),
            aperture_f_number: positive_f64(metadata.aperture_f_number),
            focal_length_mm: positive_f64(metadata.focal_length_mm),
            focal_length_35mm: positive_f64(metadata.focal_length_35mm),
            raw_dimensions: valid_dimensions(metadata.raw_dimensions),
            image_dimensions: valid_dimensions(metadata.image_dimensions),
            orientation: Some(metadata.orientation),
            gps: metadata.gps.clone(),
        }
    }
}

fn positive_f64(value: f64) -> Option<f64> {
    (value.is_finite() && value > 0.0).then_some(value)
}

const fn nonzero_i64(value: i64) -> Option<i64> {
    if value == 0 { None } else { Some(value) }
}

const fn valid_dimensions(value: ImageDimensions) -> Option<ImageDimensions> {
    if value.width == 0 || value.height == 0 {
        None
    } else {
        Some(value)
    }
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
    /// Whether the encoded preview still needs its own metadata transform or
    /// already contains display-oriented pixels. Older mirrors default to the
    /// conservative encoded-metadata path.
    #[serde(default)]
    pub pixel_orientation: RemotePreviewPixelOrientation,
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemotePreviewPixelOrientation {
    /// Apply orientation metadata embedded in the encoded preview, when any.
    #[default]
    EncodedMetadata,
    /// Pixels have already been normalized into display orientation. Applying
    /// source orientation again would be a double transform.
    DisplayOriented,
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
    pub protocol_revision: u32,
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
    pub protocol_revision: u32,
    pub authorization: String,
    pub request: Request,
}

#[cfg(test)]
mod tests;
