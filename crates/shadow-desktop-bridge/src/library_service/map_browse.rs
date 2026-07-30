//! Provider-independent Library map projection.
//!
//! The desktop passes a settled photo filter and viewport. Catalog performs bounded spatial
//! aggregation; this boundary only converts stable identities and singleton opening targets.

use anyhow::Result as AnyResult;
use shadow_catalog::{LibraryMapGrid, LibraryMapSnapshot, LibraryMapViewport};

use crate::{ffi, review_service::file_name};

use super::{LibraryService, query_contract::library_filter_from_ffi};

impl LibraryService {
    pub(crate) fn map_snapshot(
        &self,
        ffi_filter: &ffi::FfiLibraryPhotoFilter,
        viewport: LibraryMapViewport,
        grid: LibraryMapGrid,
    ) -> AnyResult<ffi::FfiLibraryMapSnapshot> {
        let filter = library_filter_from_ffi(ffi_filter)?;
        Ok(ffi_map_snapshot(
            self.catalog.library_map_snapshot(&filter, viewport, grid)?,
        ))
    }
}

fn ffi_map_snapshot(snapshot: LibraryMapSnapshot) -> ffi::FfiLibraryMapSnapshot {
    ffi::FfiLibraryMapSnapshot {
        photo_count: snapshot.photo_count,
        clusters: snapshot
            .clusters
            .into_iter()
            .map(|cluster| {
                let source_path = cluster.single_source_display_path;
                ffi::FfiLibraryMapCluster {
                    cell_x: cluster.cell_x,
                    cell_y: cluster.cell_y,
                    latitude_e7: cluster.latitude_e7,
                    longitude_e7: cluster.longitude_e7,
                    photo_count: cluster.photo_count,
                    photo_id: cluster
                        .single_photo_id
                        .map(|id| id.to_string())
                        .unwrap_or_default(),
                    representation_id: cluster
                        .single_representation_id
                        .map(|id| id.to_string())
                        .unwrap_or_default(),
                    title: file_name(&source_path),
                    source_path,
                }
            })
            .collect(),
    }
}
