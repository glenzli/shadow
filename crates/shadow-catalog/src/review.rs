//! Catalog-owned Review read projections.
//!
//! This facade defines the public records shared by every Review consumer.
//! Follow [`source`] for exact photo-source selection, [`page`] for stable grid
//! pagination, and [`projection`] for their shared SQLite row/provenance codec.

mod page;
mod projection;
mod source;

use shadow_domain::{
    AssetLocation, PhotoDecisionState, PhotoId, RawMetadataSnapshot, RepresentationId,
};

use crate::{CachedArtifactRecord, RepresentationFingerprint, TechnicalObservationSummary};

/// One immutable row for the Review grid.
#[derive(Debug, Clone, PartialEq)]
pub struct ReviewItemRecord {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location: AssetLocation,
    pub source: RepresentationFingerprint,
    pub visual: Option<CachedArtifactRecord>,
    pub metadata: Option<RawMetadataSnapshot>,
    pub technical: Option<TechnicalObservationSummary>,
    pub decision: PhotoDecisionState,
    /// Whether this photo has a durable working development recipe.
    ///
    /// This is intentionally independent from the currently selected visual:
    /// a generated proxy can represent an untouched photo, while an edited
    /// photo may still be waiting for its recipe preview to render.
    pub has_development_edits: bool,
}

/// Exact, low-frequency inspection data for one selected photo representation.
///
/// Unlike [`ReviewItemRecord`], this projection intentionally owns no gallery
/// visual, curation decision, or Library presentation state. Callers must
/// provide both identities, so a selection can never silently move to another
/// RAW-preferred representation of the same photo.
#[derive(Debug, Clone, PartialEq)]
pub struct PhotoInspectionRecord {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location: AssetLocation,
    pub source: RepresentationFingerprint,
    pub metadata: Option<RawMetadataSnapshot>,
    pub technical: Option<TechnicalObservationSummary>,
}

/// Stable keyset cursor for the Review grid's path/id ordering.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ReviewCursor {
    pub display_path: String,
    pub representation_id: RepresentationId,
}

/// One bounded Review-grid page and the cursor needed to continue it.
#[derive(Debug, Clone, PartialEq)]
pub struct ReviewPageRecord {
    pub items: Vec<ReviewItemRecord>,
    pub next_cursor: Option<ReviewCursor>,
    pub total_items: u64,
}

#[cfg(test)]
mod tests;
