//! Catalog contracts for rebuildable content-addressed image artifacts.
//!
//! This facade owns the public record vocabulary. Follow [`mutation`] for
//! source-guarded publication and invalidation, [`selection`] for current
//! presentation/reachability queries, and [`codec`] for shared persisted-value
//! decoding.

mod codec;
mod mutation;
mod selection;

use shadow_domain::{ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationId};

use crate::RepresentationFingerprint;

pub(crate) use codec::{
    digest, non_negative_u16, non_negative_u32, non_negative_u64, optional_usize, parse_byte_order,
    parse_codec, parse_role,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum CachedArtifactRole {
    /// A rendered JPEG bound to one exact durable working Recipe snapshot.
    RecipePreview,
    EmbeddedPreview,
    GeneratedProxy,
}

impl CachedArtifactRole {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::RecipePreview => "recipe_preview",
            Self::EmbeddedPreview => "embedded_preview",
            Self::GeneratedProxy => "generated_proxy",
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CachedArtifact {
    pub role: CachedArtifactRole,
    pub variant_key: String,
    pub generator_id: String,
    pub generator_version: String,
    /// Present only for an edited render. It is the exact Recipe snapshot
    /// identity that must still be named by the photo's `working` ref before
    /// this artifact is eligible for the Library grid.
    pub recipe_snapshot_digest: Option<[u8; 32]>,
    pub provider_preview_id: Option<usize>,
    pub blob_algorithm: String,
    pub blob_digest: [u8; 32],
    pub blob_byte_len: u64,
    pub codec: PreviewCodec,
    pub byte_order: PreviewByteOrder,
    pub dimensions: ImageDimensions,
    pub bits_per_channel: u16,
    pub channels: u16,
    pub created_at_ms: i64,
}

/// The exact implementation identity required for a cached artifact consumer.
///
/// Artifact rows remain rebuildable history. Consumers use this value to
/// reject output produced by an incompatible generator without deleting the
/// older row or coupling that policy to the Catalog schema.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CachedArtifactGeneratorIdentity {
    pub generator_id: String,
    pub generator_version: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RecordCachedArtifact {
    pub representation_id: RepresentationId,
    pub expected_source: RepresentationFingerprint,
    pub artifact: CachedArtifact,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RecordCachedArtifactStatus {
    Recorded,
    StaleSource,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CachedArtifactRecord {
    pub representation_id: RepresentationId,
    pub source: RepresentationFingerprint,
    pub artifact: CachedArtifact,
}

/// One content-addressed blob that is still reachable from a current Catalog
/// artifact. This is intentionally storage-neutral: the cache crate converts
/// supported algorithm/digest pairs into filesystem paths, while Catalog owns
/// the source and Recipe validity rules that decide reachability.
#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct LiveCachedArtifactBlob {
    pub algorithm: String,
    pub digest: [u8; 32],
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum InvalidateCachedArtifactStatus {
    Invalidated,
    NotCurrent,
}

#[cfg(test)]
mod tests;
