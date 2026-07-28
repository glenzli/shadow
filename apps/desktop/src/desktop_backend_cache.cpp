#include "backend/desktop_backend_private.hpp"

ExportBackend& DesktopBackend::exportBackend() noexcept {
    return impl_->export_backend;
}

BackendCacheMaintenanceInventory DesktopBackend::cacheMaintenanceInventory() const {
    const auto inventory = impl_->session->cache_maintenance_inventory();
    return {
        .catalog_live_blob_count = inventory.catalog_live_blob_count,
        .cache_blob_count = inventory.cache_blob_count,
        .cache_blob_byte_length = inventory.cache_blob_byte_len,
        .unknown_entry_count = inventory.unknown_entry_count,
        .unsupported_algorithm_count = inventory.unsupported_algorithm_count,
    };
}

BackendCacheMaintenanceSweep DesktopBackend::planCacheMaintenanceSweep() const {
    const auto sweep = impl_->session->cache_maintenance_sweep(true);
    return {
        .dry_run = sweep.dry_run,
        .catalog_live_blob_count = sweep.catalog_live_blob_count,
        .cache_blob_count = sweep.cache_blob_count,
        .cache_blob_byte_length = sweep.cache_blob_byte_len,
        .unknown_entry_count = sweep.unknown_entry_count,
        .unsupported_algorithm_count = sweep.unsupported_algorithm_count,
        .retained_blob_count = sweep.retained_blob_count,
        .recently_protected_blob_count = sweep.recently_protected_blob_count,
        .recently_protected_byte_length = sweep.recently_protected_byte_len,
        .reclaimed_blob_count = sweep.reclaimed_blob_count,
        .reclaimed_byte_length = sweep.reclaimed_byte_len,
    };
}

BackendCacheMaintenanceSweep DesktopBackend::runCacheMaintenanceSweep() const {
    const auto sweep = impl_->session->cache_maintenance_sweep(false);
    return {
        .dry_run = sweep.dry_run,
        .catalog_live_blob_count = sweep.catalog_live_blob_count,
        .cache_blob_count = sweep.cache_blob_count,
        .cache_blob_byte_length = sweep.cache_blob_byte_len,
        .unknown_entry_count = sweep.unknown_entry_count,
        .unsupported_algorithm_count = sweep.unsupported_algorithm_count,
        .retained_blob_count = sweep.retained_blob_count,
        .recently_protected_blob_count = sweep.recently_protected_blob_count,
        .recently_protected_byte_length = sweep.recently_protected_byte_len,
        .reclaimed_blob_count = sweep.reclaimed_blob_count,
        .reclaimed_byte_length = sweep.reclaimed_byte_len,
    };
}
