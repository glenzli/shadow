//! Conservative Catalog-backed cache maintenance for a desktop session.
//!
//! The Catalog decides which artifacts remain reachable from current source
//! and Recipe identities. The content-addressed store decides which canonical
//! filesystem blobs may be removed. Keeping those decisions here prevents a
//! future settings UI from recreating its own, subtly different retention
//! policy.

use std::collections::BTreeSet;

use anyhow::{Context, Result as AnyResult};
use shadow_cache::{BlobDigest, CacheInventory, CacheSweepReport, ContentAddressedStore};
use shadow_catalog::{CatalogHandle, LiveCachedArtifactBlob};

/// Read-only cache state suitable for a settings panel or maintenance prompt.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct CacheMaintenanceInventory {
    pub(crate) cache: CacheInventory,
    /// Count of distinct current Catalog rows before algorithm filtering.
    pub(crate) catalog_live_blob_count: usize,
    /// Current Catalog blob algorithms that this local cache root cannot own.
    /// They are retained in Catalog but never become deletion candidates here.
    pub(crate) unsupported_algorithms: Vec<String>,
}

/// Result of one conservative, optionally dry-run sweep.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct CacheMaintenanceSweep {
    pub(crate) cache: CacheSweepReport,
    pub(crate) catalog_live_blob_count: usize,
    pub(crate) unsupported_algorithms: Vec<String>,
}

/// Session facade joining durable Catalog reachability with the local cache
/// root. It intentionally has no autonomous timer: cache eviction is a user
/// controlled maintenance action until quota policy is explicitly designed.
#[derive(Debug, Clone)]
pub(crate) struct CacheMaintenanceService {
    catalog: CatalogHandle,
    cache: ContentAddressedStore,
}

impl CacheMaintenanceService {
    pub(crate) fn new(catalog: CatalogHandle, cache: ContentAddressedStore) -> Self {
        Self { catalog, cache }
    }

    /// Reads Catalog reachability and local inventory without deleting files.
    pub(crate) fn inventory(&self) -> AnyResult<CacheMaintenanceInventory> {
        let live = self.live_catalog_blobs()?;
        Ok(CacheMaintenanceInventory {
            cache: self
                .cache
                .inventory()
                .context("inspect local Shadow cache")?,
            catalog_live_blob_count: live.catalog_live_blob_count,
            unsupported_algorithms: live.unsupported_algorithms,
        })
    }

    /// Performs a conservative cache sweep. `dry_run` calculates the exact
    /// canonical candidates without deleting them; unknown filesystem entries
    /// and Catalog rows using unsupported algorithms are never removed.
    pub(crate) fn sweep(&self, dry_run: bool) -> AnyResult<CacheMaintenanceSweep> {
        let live = self.live_catalog_blobs()?;
        Ok(CacheMaintenanceSweep {
            cache: self
                .cache
                .sweep_unreferenced(&live.retained_blake3, dry_run)
                .context("sweep unreferenced Shadow cache blobs")?,
            catalog_live_blob_count: live.catalog_live_blob_count,
            unsupported_algorithms: live.unsupported_algorithms,
        })
    }

    fn live_catalog_blobs(&self) -> AnyResult<LiveCatalogBlobs> {
        let blobs = self
            .catalog
            .live_cached_artifact_blobs()
            .context("read live cached-artifact identities from Catalog")?;
        Ok(partition_catalog_blobs(blobs))
    }
}

#[derive(Debug)]
struct LiveCatalogBlobs {
    retained_blake3: BTreeSet<BlobDigest>,
    catalog_live_blob_count: usize,
    unsupported_algorithms: Vec<String>,
}

fn partition_catalog_blobs(blobs: Vec<LiveCachedArtifactBlob>) -> LiveCatalogBlobs {
    let catalog_live_blob_count = blobs.len();
    let mut retained_blake3 = BTreeSet::new();
    let mut unsupported_algorithms = BTreeSet::new();
    for blob in blobs {
        if blob.algorithm == BlobDigest::from_bytes(blob.digest).algorithm() {
            retained_blake3.insert(BlobDigest::from_bytes(blob.digest));
        } else {
            unsupported_algorithms.insert(blob.algorithm);
        }
    }
    LiveCatalogBlobs {
        retained_blake3,
        catalog_live_blob_count,
        unsupported_algorithms: unsupported_algorithms.into_iter().collect(),
    }
}

#[cfg(test)]
mod tests;
