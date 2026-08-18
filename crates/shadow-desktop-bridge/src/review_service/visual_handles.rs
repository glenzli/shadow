//! Signed Review and Library grid-visual authorization.
//!
//! This owner keeps both durable cached-artifact handles and session-preview
//! handles under one signing key. It also owns grid selection, stale embedded
//! preview retirement, payload schemas, codecs, and the two authorized loading
//! routes. The parent Review service only composes dependencies and dispatches
//! visual tickets.

use std::sync::Arc;

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use serde::{Deserialize, Serialize};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, InvalidateCachedArtifactStatus,
    RepresentationFingerprint,
};
use shadow_domain::{ImageDimensions, PhotoId, PreviewByteOrder, PreviewCodec, RepresentationId};
use uuid::Uuid;

use crate::{
    digest_hex::encode_hex,
    ffi,
    session_preview_store::{SessionPreviewDescriptor, SessionPreviewStore},
};

use super::ReviewService;

const GRID_VISUAL_HANDLE_PREFIX: &str = "shadow-grid-visual-v1.";
const SESSION_GRID_VISUAL_HANDLE_PREFIX: &str = "shadow-grid-session-v1.";
const GRID_VISUAL_HANDLE_SCHEMA_VERSION: u8 = 1;
const SESSION_GRID_VISUAL_HANDLE_SCHEMA_VERSION: u8 = 1;
const MAX_GRID_VISUAL_PAYLOAD_BYTES: usize = 16 * 1_024;
const MAX_PREVIEW_REFRESH_TICKETS: usize = 512;

/// The exact cached artifact chosen for a Review visual.
#[derive(Debug, Clone)]
pub(crate) struct ReviewVisualSelection {
    pub(crate) photo_id: PhotoId,
    pub(crate) record: CachedArtifactRecord,
}

/// Opaque grid visual contract shared by Review and the photo-first Library.
///
/// The signed handle and transient embedded-preview fallback deliberately stay
/// owned by [`ReviewService`]. Library owns durable query projection, but it
/// must not duplicate the loader's authorization or session-preview policy.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct GridVisualPresentation {
    pub(crate) handle: String,
    pub(crate) role: String,
    pub(crate) dimensions: ImageDimensions,
}

/// Session-bound state shared by the durable and transient handle families.
#[derive(Debug)]
pub(super) struct SignedVisualHandles {
    session_previews: Arc<SessionPreviewStore>,
    signing_key: [u8; 32],
}

impl SignedVisualHandles {
    pub(super) fn new(session_previews: Arc<SessionPreviewStore>) -> Self {
        Self {
            session_previews,
            signing_key: new_visual_signing_key(),
        }
    }
}

/// The three visual-ticket families understood by the Review facade.
pub(super) enum VisualTicketRoute {
    SessionPreview,
    DurableGrid,
    Comparison,
}

pub(super) fn ticket_route(ticket: &str) -> VisualTicketRoute {
    if ticket.starts_with(SESSION_GRID_VISUAL_HANDLE_PREFIX) {
        VisualTicketRoute::SessionPreview
    } else if ticket.starts_with(GRID_VISUAL_HANDLE_PREFIX) {
        VisualTicketRoute::DurableGrid
    } else {
        VisualTicketRoute::Comparison
    }
}

impl ReviewService {
    /// Converts one current Catalog artifact into the exact short-lived grid
    /// presentation that Qt may request. This is intentionally shared by
    /// Review and Library so an embedded camera preview never bypasses the
    /// same invalidation and session-local fallback policy in one surface.
    pub(crate) fn grid_visual(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
        source: RepresentationFingerprint,
        visual: Option<&CachedArtifactRecord>,
    ) -> AnyResult<Option<GridVisualPresentation>> {
        let current_visual = visual.filter(|visual| {
            visual.representation_id == representation_id && visual.source == source
        });
        if let Some(visual) = current_visual
            .filter(|visual| visual.artifact.role == CachedArtifactRole::EmbeddedPreview)
        {
            // Older development builds persisted embedded camera previews. They
            // are not a valid Shadow rendering contract and can be visibly stale
            // after decoder/color-pipeline changes, so retire the exact obsolete
            // Catalog row opportunistically. The blob is content-addressed and
            // may be reclaimed by normal cache garbage collection later.
            let _ = self.catalog.invalidate_cached_artifact(visual);
        }
        if let Some(visual) = current_visual
            .filter(|visual| visual.artifact.role != CachedArtifactRole::EmbeddedPreview)
        {
            return Ok(Some(GridVisualPresentation {
                handle: self.encode_grid_visual_handle(&ReviewVisualSelection {
                    photo_id,
                    record: visual.clone(),
                })?,
                role: role_name(visual.artifact.role).to_owned(),
                dimensions: visual.artifact.dimensions,
            }));
        }
        let Some(descriptor) = self
            .visual_handles
            .session_previews
            .lookup(representation_id, source)
        else {
            return Ok(None);
        };
        Ok(Some(GridVisualPresentation {
            handle: self.encode_session_grid_visual_handle(descriptor)?,
            role: "embedded".to_owned(),
            dimensions: descriptor.dimensions,
        }))
    }

    pub(super) fn load_session_grid_visual(
        &self,
        ticket: &str,
    ) -> AnyResult<ffi::FfiVisualPayload> {
        let descriptor = self.decode_session_grid_visual_handle(ticket)?;
        let bytes = self
            .visual_handles
            .session_previews
            .load(descriptor)
            .ok_or_else(|| anyhow!("embedded preview has expired from this Shadow session"))?;
        Ok(ffi::FfiVisualPayload {
            bytes: bytes.as_ref().to_vec(),
            requires_frame_receipt: false,
        })
    }

    pub(super) fn load_durable_grid_visual(
        &self,
        ticket: &str,
    ) -> AnyResult<ffi::FfiVisualPayload> {
        let selection = self.decode_grid_visual_handle(ticket)?;
        Ok(ffi::FfiVisualPayload {
            bytes: self.loader.load_bytes(&selection.record)?,
            requires_frame_receipt: false,
        })
    }

    /// Invalidates only the exact durable proxy references selected by the
    /// current Library session. The source original remains immutable and the
    /// subsequent folder scan is responsible for scheduling a replacement.
    ///
    /// Session-only embedded previews cannot participate: they have no durable
    /// Catalog cache reference, so treating them as refreshable would make the
    /// UI promise work that cannot be completed safely.
    pub(crate) fn invalidate_selected_preview_visuals(
        &self,
        tickets: Vec<String>,
    ) -> AnyResult<u32> {
        if tickets.is_empty() {
            bail!("select at least one durable preview to refresh");
        }
        if tickets.len() > MAX_PREVIEW_REFRESH_TICKETS {
            bail!("selected preview refresh exceeds its 512-photo limit");
        }

        // Validate every requested handle before mutating a cache row. A mixed
        // selection can otherwise invalidate its early durable rows before a
        // later session-only handle is rejected.
        let selections = tickets
            .into_iter()
            .map(|ticket| {
                if !matches!(ticket_route(&ticket), VisualTicketRoute::DurableGrid) {
                    bail!("selected preview refresh requires durable generated proxies");
                }
                self.decode_grid_visual_handle(&ticket)
            })
            .collect::<AnyResult<Vec<_>>>()?;

        let mut invalidated = 0_u32;
        for selection in selections {
            if self.catalog.invalidate_cached_artifact(&selection.record)?
                == InvalidateCachedArtifactStatus::Invalidated
            {
                invalidated = invalidated.saturating_add(1);
            }
        }
        Ok(invalidated)
    }

    pub(crate) fn encode_grid_visual_handle(
        &self,
        selection: &ReviewVisualSelection,
    ) -> AnyResult<String> {
        let payload = SignedGridVisualPayload::from_selection(selection)?;
        let payload = serde_json::to_vec(&payload).context("encode Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        let signature = blake3::keyed_hash(&self.visual_handles.signing_key, &payload);
        Ok(format!(
            "{GRID_VISUAL_HANDLE_PREFIX}{}.{}",
            encode_hex(&payload),
            signature.to_hex()
        ))
    }

    pub(super) fn decode_grid_visual_handle(
        &self,
        handle: &str,
    ) -> AnyResult<ReviewVisualSelection> {
        let encoded = handle
            .strip_prefix(GRID_VISUAL_HANDLE_PREFIX)
            .ok_or_else(|| anyhow!("invalid Review grid visual handle prefix"))?;
        let (payload_hex, signature_hex) = encoded
            .split_once('.')
            .ok_or_else(|| anyhow!("malformed Review grid visual handle"))?;
        if payload_hex.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES.saturating_mul(2) {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        if signature_hex.len() != 64 || !is_lower_hex(signature_hex) {
            bail!("malformed Review grid visual handle signature");
        }
        let payload = decode_hex(payload_hex).context("decode Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        let supplied_signature =
            decode_hex_32(signature_hex).context("decode Review grid visual handle signature")?;
        let expected_signature = blake3::keyed_hash(&self.visual_handles.signing_key, &payload);
        if !constant_time_eq(expected_signature.as_bytes(), &supplied_signature) {
            bail!("Review grid visual handle signature is invalid for this session");
        }
        let payload: SignedGridVisualPayload =
            serde_json::from_slice(&payload).context("parse Review grid visual handle")?;
        payload.into_selection()
    }

    fn encode_session_grid_visual_handle(
        &self,
        descriptor: SessionPreviewDescriptor,
    ) -> AnyResult<String> {
        let payload = SignedSessionGridVisualPayload::from_descriptor(descriptor);
        let payload =
            serde_json::to_vec(&payload).context("encode session Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("session Review grid visual handle payload exceeds its size limit");
        }
        let signature = blake3::keyed_hash(&self.visual_handles.signing_key, &payload);
        Ok(format!(
            "{SESSION_GRID_VISUAL_HANDLE_PREFIX}{}.{}",
            encode_hex(&payload),
            signature.to_hex()
        ))
    }

    fn decode_session_grid_visual_handle(
        &self,
        handle: &str,
    ) -> AnyResult<SessionPreviewDescriptor> {
        let encoded = handle
            .strip_prefix(SESSION_GRID_VISUAL_HANDLE_PREFIX)
            .ok_or_else(|| anyhow!("invalid session Review grid visual handle prefix"))?;
        let (payload_hex, signature_hex) = encoded
            .split_once('.')
            .ok_or_else(|| anyhow!("malformed session Review grid visual handle"))?;
        if payload_hex.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES.saturating_mul(2) {
            bail!("session Review grid visual handle payload exceeds its size limit");
        }
        if signature_hex.len() != 64 || !is_lower_hex(signature_hex) {
            bail!("malformed session Review grid visual handle signature");
        }
        let payload =
            decode_hex(payload_hex).context("decode session Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("session Review grid visual handle payload exceeds its size limit");
        }
        let supplied_signature = decode_hex_32(signature_hex)
            .context("decode session Review grid visual handle signature")?;
        let expected_signature = blake3::keyed_hash(&self.visual_handles.signing_key, &payload);
        if !constant_time_eq(expected_signature.as_bytes(), &supplied_signature) {
            bail!("session Review grid visual handle signature is invalid for this session");
        }
        let payload: SignedSessionGridVisualPayload =
            serde_json::from_slice(&payload).context("parse session Review grid visual handle")?;
        payload.into_descriptor()
    }
}

/// Signed session-only reference to encoded camera-preview bytes.
///
/// This is intentionally much smaller than [`SignedGridVisualPayload`]: it
/// authenticates only the identity needed to retrieve a value from the
/// process-local store, never a durable Catalog/cache artifact.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct SignedSessionGridVisualPayload {
    schema_version: u8,
    representation_id: String,
    source_byte_len: u64,
    source_modified_at_ms: Option<i64>,
    revision: u64,
    codec: String,
    byte_order: String,
    width: u32,
    height: u32,
    bits_per_channel: u16,
    channels: u16,
}

impl SignedSessionGridVisualPayload {
    fn from_descriptor(descriptor: SessionPreviewDescriptor) -> Self {
        Self {
            schema_version: SESSION_GRID_VISUAL_HANDLE_SCHEMA_VERSION,
            representation_id: descriptor.representation_id.to_string(),
            source_byte_len: descriptor.source.byte_len,
            source_modified_at_ms: descriptor.source.modified_at_ms,
            revision: descriptor.revision,
            codec: descriptor.codec.as_str().to_owned(),
            byte_order: descriptor.byte_order.as_str().to_owned(),
            width: descriptor.dimensions.width,
            height: descriptor.dimensions.height,
            bits_per_channel: descriptor.bits_per_channel,
            channels: descriptor.channels,
        }
    }

    fn into_descriptor(self) -> AnyResult<SessionPreviewDescriptor> {
        if self.schema_version != SESSION_GRID_VISUAL_HANDLE_SCHEMA_VERSION {
            bail!(
                "unsupported session Review grid visual handle schema {}",
                self.schema_version
            );
        }
        let representation_id = self
            .representation_id
            .parse()
            .context("parse representation id in session Review grid visual handle")?;
        let codec = match self.codec.as_str() {
            "unknown" => PreviewCodec::Unknown,
            "jpeg" => PreviewCodec::Jpeg,
            "bitmap" => PreviewCodec::Bitmap,
            "jpeg_xl" => PreviewCodec::JpegXl,
            "h265" => PreviewCodec::H265,
            other => bail!("unsupported session Review visual codec {other:?}"),
        };
        let byte_order = match self.byte_order.as_str() {
            "not_applicable" => PreviewByteOrder::NotApplicable,
            "native" => PreviewByteOrder::Native,
            "little_endian" => PreviewByteOrder::LittleEndian,
            "big_endian" => PreviewByteOrder::BigEndian,
            other => bail!("unsupported session Review visual byte order {other:?}"),
        };
        Ok(SessionPreviewDescriptor {
            representation_id,
            source: RepresentationFingerprint {
                byte_len: self.source_byte_len,
                modified_at_ms: self.source_modified_at_ms,
            },
            revision: self.revision,
            codec,
            byte_order,
            dimensions: ImageDimensions {
                width: self.width,
                height: self.height,
            },
            bits_per_channel: self.bits_per_channel,
            channels: self.channels,
        })
    }
}

/// Serializable mirror of the Catalog record carried by a signed grid handle.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct SignedGridVisualPayload {
    schema_version: u8,
    photo_id: String,
    representation_id: String,
    source_byte_len: u64,
    source_modified_at_ms: Option<i64>,
    role: String,
    variant_key: String,
    generator_id: String,
    generator_version: String,
    recipe_snapshot_digest_hex: Option<String>,
    provider_preview_id: Option<u64>,
    blob_algorithm: String,
    blob_digest_hex: String,
    blob_byte_len: u64,
    codec: String,
    byte_order: String,
    width: u32,
    height: u32,
    bits_per_channel: u16,
    channels: u16,
    created_at_ms: i64,
}

impl SignedGridVisualPayload {
    fn from_selection(selection: &ReviewVisualSelection) -> AnyResult<Self> {
        let record = &selection.record;
        Ok(Self {
            schema_version: GRID_VISUAL_HANDLE_SCHEMA_VERSION,
            photo_id: selection.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            source_byte_len: record.source.byte_len,
            source_modified_at_ms: record.source.modified_at_ms,
            role: record.artifact.role.as_str().to_owned(),
            variant_key: record.artifact.variant_key.clone(),
            generator_id: record.artifact.generator_id.clone(),
            generator_version: record.artifact.generator_version.clone(),
            recipe_snapshot_digest_hex: record
                .artifact
                .recipe_snapshot_digest
                .as_ref()
                .map(|digest| encode_hex(digest)),
            provider_preview_id: record
                .artifact
                .provider_preview_id
                .map(u64::try_from)
                .transpose()
                .context("provider preview id does not fit Review provenance")?,
            blob_algorithm: record.artifact.blob_algorithm.clone(),
            blob_digest_hex: encode_hex(&record.artifact.blob_digest),
            blob_byte_len: record.artifact.blob_byte_len,
            codec: record.artifact.codec.as_str().to_owned(),
            byte_order: record.artifact.byte_order.as_str().to_owned(),
            width: record.artifact.dimensions.width,
            height: record.artifact.dimensions.height,
            bits_per_channel: record.artifact.bits_per_channel,
            channels: record.artifact.channels,
            created_at_ms: record.artifact.created_at_ms,
        })
    }

    fn into_selection(self) -> AnyResult<ReviewVisualSelection> {
        if self.schema_version != GRID_VISUAL_HANDLE_SCHEMA_VERSION {
            bail!(
                "unsupported Review grid visual handle schema {}",
                self.schema_version
            );
        }
        let photo_id = self
            .photo_id
            .parse()
            .context("parse photo id in Review grid visual handle")?;
        let representation_id = self
            .representation_id
            .parse()
            .context("parse representation id in Review grid visual handle")?;
        let role = match self.role.as_str() {
            "recipe_preview" => CachedArtifactRole::RecipePreview,
            "embedded_preview" => CachedArtifactRole::EmbeddedPreview,
            "generated_proxy" => CachedArtifactRole::GeneratedProxy,
            other => bail!("unsupported Review visual artifact role {other:?}"),
        };
        let codec = match self.codec.as_str() {
            "unknown" => PreviewCodec::Unknown,
            "jpeg" => PreviewCodec::Jpeg,
            "bitmap" => PreviewCodec::Bitmap,
            "jpeg_xl" => PreviewCodec::JpegXl,
            "h265" => PreviewCodec::H265,
            other => bail!("unsupported Review visual codec {other:?}"),
        };
        let byte_order = match self.byte_order.as_str() {
            "not_applicable" => PreviewByteOrder::NotApplicable,
            "native" => PreviewByteOrder::Native,
            "little_endian" => PreviewByteOrder::LittleEndian,
            "big_endian" => PreviewByteOrder::BigEndian,
            other => bail!("unsupported Review visual byte order {other:?}"),
        };
        let recipe_snapshot_digest = self
            .recipe_snapshot_digest_hex
            .as_deref()
            .map(decode_hex_32)
            .transpose()
            .context("decode Recipe snapshot digest in Review visual handle")?;
        Ok(ReviewVisualSelection {
            photo_id,
            record: CachedArtifactRecord {
                representation_id,
                source: RepresentationFingerprint {
                    byte_len: self.source_byte_len,
                    modified_at_ms: self.source_modified_at_ms,
                },
                artifact: CachedArtifact {
                    role,
                    variant_key: self.variant_key,
                    generator_id: self.generator_id,
                    generator_version: self.generator_version,
                    recipe_snapshot_digest,
                    provider_preview_id: self
                        .provider_preview_id
                        .map(usize::try_from)
                        .transpose()
                        .context("provider preview id does not fit this platform")?,
                    blob_algorithm: self.blob_algorithm,
                    blob_digest: decode_hex_32(&self.blob_digest_hex)
                        .context("decode Review visual blob digest")?,
                    blob_byte_len: self.blob_byte_len,
                    codec,
                    byte_order,
                    dimensions: ImageDimensions {
                        width: self.width,
                        height: self.height,
                    },
                    bits_per_channel: self.bits_per_channel,
                    channels: self.channels,
                    created_at_ms: self.created_at_ms,
                },
            },
        })
    }
}

fn new_visual_signing_key() -> [u8; 32] {
    let first = Uuid::now_v7();
    let second = Uuid::now_v7();
    let mut key = [0_u8; 32];
    key[..16].copy_from_slice(first.as_bytes());
    key[16..].copy_from_slice(second.as_bytes());
    key
}

fn decode_hex(encoded: &str) -> AnyResult<Vec<u8>> {
    if !encoded.len().is_multiple_of(2) || !is_lower_hex(encoded) {
        bail!("hex value must contain an even number of lowercase hexadecimal characters");
    }
    encoded
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| Ok((hex_nibble(pair[0])? << 4) | hex_nibble(pair[1])?))
        .collect()
}

fn decode_hex_32(encoded: &str) -> AnyResult<[u8; 32]> {
    if encoded.len() != 64 {
        bail!("digest must contain exactly 64 hexadecimal characters");
    }
    let bytes = decode_hex(encoded)?;
    bytes
        .try_into()
        .map_err(|_| anyhow!("digest must contain exactly 32 bytes"))
}

fn hex_nibble(byte: u8) -> AnyResult<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        _ => Err(anyhow::Error::msg("invalid lowercase hexadecimal digit")),
    }
}

pub(super) fn is_lower_hex(value: &str) -> bool {
    value
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn constant_time_eq(left: &[u8], right: &[u8]) -> bool {
    left.len() == right.len()
        && left
            .iter()
            .zip(right)
            .fold(0_u8, |difference, (left, right)| {
                difference | (left ^ right)
            })
            == 0
}

const fn role_name(role: CachedArtifactRole) -> &'static str {
    match role {
        CachedArtifactRole::RecipePreview => "recipe",
        CachedArtifactRole::EmbeddedPreview => "embedded",
        CachedArtifactRole::GeneratedProxy => "proxy",
    }
}
