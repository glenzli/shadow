#pragma once

#include <cstdint>

// Conservative cache inventory and confirmed-maintenance receipts.
/// Read-only cache footprint and Catalog reachability. Unknown cache entries
/// and unsupported future digest algorithms are deliberately reported instead
/// of treated as garbage.
struct BackendCacheMaintenanceInventory final {
    std::uint64_t catalog_live_blob_count = 0;
    std::uint64_t cache_blob_count = 0;
    std::uint64_t cache_blob_byte_length = 0;
    std::uint64_t unknown_entry_count = 0;
    std::uint32_t unsupported_algorithm_count = 0;
};

/// Result of an explicit cache maintenance plan or confirmed sweep. When
/// `dry_run` is true, `reclaimed_*` names candidates only and no filesystem
/// mutation has occurred.
struct BackendCacheMaintenanceSweep final {
    bool dry_run = true;
    std::uint64_t catalog_live_blob_count = 0;
    std::uint64_t cache_blob_count = 0;
    std::uint64_t cache_blob_byte_length = 0;
    std::uint64_t unknown_entry_count = 0;
    std::uint32_t unsupported_algorithm_count = 0;
    std::uint64_t retained_blob_count = 0;
    std::uint64_t recently_protected_blob_count = 0;
    std::uint64_t recently_protected_byte_length = 0;
    std::uint64_t reclaimed_blob_count = 0;
    std::uint64_t reclaimed_byte_length = 0;
};
