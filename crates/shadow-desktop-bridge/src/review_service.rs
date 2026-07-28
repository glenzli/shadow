//! Stateful Review operations for the desktop bridge.
//!
//! This service composes the session-local Review owners while the Catalog
//! remains the durable source of photo decisions and feedback facts. Qt DTO
//! shaping stays in the bridge facade.
//!
//! Navigate to [`browse`] for gallery-page/metadata projection, [`decisions`]
//! for the append-only human flag/rating lifecycle, [`comparison`] for exact
//! visual presentation plus reversible pairwise feedback, and
//! [`visual_handles`] for signed durable/session grid authorization shared by
//! Review and Library.

use std::sync::Arc;

use anyhow::Result as AnyResult;
use shadow_catalog::CatalogHandle;
use shadow_core::CachedArtifactLoader;

use crate::{ffi, session_preview_store::SessionPreviewStore};

mod browse;
mod comparison;
mod decisions;
mod visual_handles;

pub(crate) use browse::file_name;
#[cfg(test)]
pub(crate) use browse::parse_cursor;
#[cfg(test)]
pub(crate) use comparison::{
    REVIEW_COMPARE_DECODER_ID, REVIEW_COMPARE_PIXEL_FORMAT, REVIEW_COMPARE_PIXEL_HASH_ALGORITHM,
    REVIEW_COMPARE_SURFACE_ID, REVIEW_COMPARE_SURFACE_REVISION,
};
pub(crate) use decisions::ffi_decision_flag;
pub(crate) use visual_handles::{GridVisualPresentation, ReviewVisualSelection};

/// Session-local review state. Every durable mutation still goes through the
/// Catalog actor, so this type can be discarded whenever a desktop session ends.
#[derive(Debug)]
pub(crate) struct ReviewService {
    catalog: CatalogHandle,
    loader: CachedArtifactLoader,
    visual_handles: visual_handles::SignedVisualHandles,
    comparison: comparison::ReviewComparisonSession,
}

impl ReviewService {
    pub(crate) fn new_with_session_previews(
        catalog: CatalogHandle,
        loader: CachedArtifactLoader,
        session_previews: Arc<SessionPreviewStore>,
    ) -> Self {
        // Preserve the original session identity ordering: the public feedback
        // session is minted before the two UUIDs that seed handle signatures.
        let comparison = comparison::ReviewComparisonSession::new();
        let visual_handles = visual_handles::SignedVisualHandles::new(session_previews);
        Self {
            catalog,
            loader,
            visual_handles,
            comparison,
        }
    }

    pub(crate) fn load_visual(&self, ticket: &str) -> AnyResult<ffi::FfiVisualPayload> {
        match visual_handles::ticket_route(ticket) {
            visual_handles::VisualTicketRoute::SessionPreview => {
                self.load_session_grid_visual(ticket)
            }
            visual_handles::VisualTicketRoute::DurableGrid => self.load_durable_grid_visual(ticket),
            visual_handles::VisualTicketRoute::Comparison => self.load_comparison_visual(ticket),
        }
    }
}
