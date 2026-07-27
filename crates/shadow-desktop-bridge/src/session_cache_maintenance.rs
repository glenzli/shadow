//! Desktop-session CXX mapping and delegation for explicit cache maintenance.

use anyhow::Result as AnyResult;

use super::{
    DesktopSession,
    cache_maintenance_service::{CacheMaintenanceInventory, CacheMaintenanceSweep},
    ffi,
};

fn ffi_cache_maintenance_inventory(
    source: CacheMaintenanceInventory,
) -> ffi::FfiCacheMaintenanceInventory {
    let CacheMaintenanceInventory {
        cache,
        catalog_live_blob_count,
        unsupported_algorithms,
    } = source;
    ffi::FfiCacheMaintenanceInventory {
        catalog_live_blob_count: u64::try_from(catalog_live_blob_count).unwrap_or(u64::MAX),
        cache_blob_count: u64::try_from(cache.blobs.len()).unwrap_or(u64::MAX),
        cache_blob_byte_len: cache.total_byte_len,
        unknown_entry_count: u64::try_from(cache.unknown_relative_paths.len()).unwrap_or(u64::MAX),
        unsupported_algorithm_count: u32::try_from(unsupported_algorithms.len())
            .unwrap_or(u32::MAX),
    }
}

fn ffi_cache_maintenance_sweep(source: CacheMaintenanceSweep) -> ffi::FfiCacheMaintenanceSweep {
    let CacheMaintenanceSweep {
        cache,
        catalog_live_blob_count,
        unsupported_algorithms,
    } = source;
    let inventory = cache.inventory;
    ffi::FfiCacheMaintenanceSweep {
        dry_run: cache.dry_run,
        catalog_live_blob_count: u64::try_from(catalog_live_blob_count).unwrap_or(u64::MAX),
        cache_blob_count: u64::try_from(inventory.blobs.len()).unwrap_or(u64::MAX),
        cache_blob_byte_len: inventory.total_byte_len,
        unknown_entry_count: u64::try_from(inventory.unknown_relative_paths.len())
            .unwrap_or(u64::MAX),
        unsupported_algorithm_count: u32::try_from(unsupported_algorithms.len())
            .unwrap_or(u32::MAX),
        retained_blob_count: cache.retained_blob_count,
        recently_protected_blob_count: cache.recently_protected_blob_count,
        recently_protected_byte_len: cache.recently_protected_byte_len,
        reclaimed_blob_count: u64::try_from(cache.reclaimed.len()).unwrap_or(u64::MAX),
        reclaimed_byte_len: cache.reclaimed_byte_len,
    }
}

impl DesktopSession {
    /// Reads cache reachability without deleting anything. The desktop must
    /// call `cache_maintenance_sweep(true)` before it presents a confirmation
    /// for a destructive sweep.
    pub(crate) fn cache_maintenance_inventory(
        &self,
    ) -> AnyResult<ffi::FfiCacheMaintenanceInventory> {
        Ok(ffi_cache_maintenance_inventory(
            self.cache_maintenance.inventory()?,
        ))
    }

    /// Performs the explicitly selected conservative maintenance action. A
    /// false `dry_run` remains safe against current Recipe/source references,
    /// unknown formats, and blobs inside the short publication grace period.
    pub(crate) fn cache_maintenance_sweep(
        &self,
        dry_run: bool,
    ) -> AnyResult<ffi::FfiCacheMaintenanceSweep> {
        Ok(ffi_cache_maintenance_sweep(
            self.cache_maintenance.sweep(dry_run)?,
        ))
    }
}
