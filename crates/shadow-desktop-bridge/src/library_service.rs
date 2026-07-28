//! Photo-first Library services for the desktop bridge.
//!
//! This facade keeps the stable Catalog handle and routes each protocol family
//! to its semantic owner:
//!
//! - [`browse`] owns photo pages, facets, and signed visual presentation;
//! - [`organization`] owns albums, memberships, and photo-affinity state;
//! - [`source_health`] owns observational scan and missing-location views;
//! - [`query_contract`] owns the typed CXX filter/facet/cursor conversion shared
//!   by browsing and smart albums.
//!
//! Review remains a separate, session-local visual and comparison service:
//! replacing a Library grid query must not change the evidence or
//! signed-preview contract.

mod browse;
mod organization;
mod query_contract;
mod source_health;

use shadow_catalog::CatalogHandle;

/// Stateless entry facade for the Catalog's photo-first Library protocols.
///
/// The facade owns only shared composition. Follow the responsibility modules
/// above for behavior and wire conversion.
#[derive(Debug, Clone)]
pub(crate) struct LibraryService {
    catalog: CatalogHandle,
}

impl LibraryService {
    pub(crate) fn new(catalog: CatalogHandle) -> Self {
        Self { catalog }
    }
}
