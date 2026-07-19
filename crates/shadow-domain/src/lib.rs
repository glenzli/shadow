//! Stable domain types shared by Shadow's UI, catalog, renderer, and workers.

mod asset;
mod ids;

pub use asset::{AssetLocation, LocationStatus, Platform, RepresentationKind};
pub use ids::{
    EntityId, GroupId, LayerId, LocationId, PhotoId, RecipeCommitId, RecipeId, RepresentationId,
    ShootId, StyleId,
};
