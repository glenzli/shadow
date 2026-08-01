//! Photo-first Library services for the desktop bridge.
//!
//! This facade keeps the stable Catalog handle and routes each protocol family
//! to its semantic owner:
//!
//! - [`browse`] owns photo pages, facets, and signed visual presentation;
//! - [`organization`] owns albums, memberships, and photo-affinity state;
//! - [`keywords`] owns the hierarchy and committed photo assignments;
//! - [`lifecycle`] owns non-destructive removal from active Library projections;
//! - [`metadata`] owns observed/effective metadata projection and routes
//!   independent capture-time and GPX preview lifecycles;
//! - [`map_browse`] owns provider-independent spatial aggregation;
//! - [`place_resolution`] owns provider-neutral unresolved coordinate and result projection;
//! - [`source_health`] owns reversible source removal plus observational scan
//!   and missing-location views;
//! - [`query_contract`] owns the typed CXX filter/facet/cursor conversion shared
//!   by browsing and smart albums.
//!
//! Review remains a separate, session-local visual and comparison service:
//! replacing a Library grid query must not change the evidence or
//! signed-preview contract.

mod browse;
mod keywords;
mod lifecycle;
mod map_browse;
mod metadata;
mod organization;
mod place_resolution;
mod query_contract;
mod source_health;

use std::sync::{Arc, Mutex};

use shadow_catalog::CatalogHandle;

/// Stateless entry facade for the Catalog's photo-first Library protocols.
///
/// The facade owns only shared composition. Follow the responsibility modules
/// above for behavior and wire conversion.
#[derive(Debug, Clone)]
pub(crate) struct LibraryService {
    catalog: CatalogHandle,
    gpx_previews: Arc<Mutex<metadata::GpxPreviewRegistry>>,
    capture_time_previews: Arc<Mutex<metadata::CaptureTimePreviewRegistry>>,
}

impl LibraryService {
    pub(crate) fn new(catalog: CatalogHandle) -> Self {
        Self {
            catalog,
            gpx_previews: Arc::new(Mutex::new(metadata::GpxPreviewRegistry::default())),
            capture_time_previews: Arc::new(Mutex::new(
                metadata::CaptureTimePreviewRegistry::default(),
            )),
        }
    }
}
