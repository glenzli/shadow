//! Stable domain types shared by Shadow's UI, catalog, renderer, and workers.

mod asset;
mod decode;
mod ids;

pub use asset::{AssetLocation, LocationStatus, Platform, RepresentationKind};
pub use decode::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PreviewCodec,
    PreviewDescriptorSnapshot, RawMetadataSnapshot,
};
pub use ids::{
    EntityId, GroupId, ImportSessionId, LayerId, LocationId, PhotoId, RecipeCommitId, RecipeId,
    RepresentationId, ShootId, StyleId,
};
