#pragma once

#include <QString>

#include <cstdint>

/// Cheap filesystem inventory used before Shadow starts a full Library scan.
///
/// This probe only walks directory entries and classifies file suffixes. It
/// never opens or decodes a photo, hashes content, or mutates the Catalog.
struct LibrarySourceQuickProbe final {
    bool root_available = false;
    bool inventory_complete = false;
    std::uint64_t supported_files = 0;

    bool operator==(const LibrarySourceQuickProbe&) const = default;
};

[[nodiscard]] bool isLibraryPhotoPath(const QString& path);
[[nodiscard]] LibrarySourceQuickProbe probeLibrarySource(const QString& root_path);
