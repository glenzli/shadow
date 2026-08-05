#pragma once

#include "review_types.hpp"

#include <QString>
#include <QVector>

#include <cstdint>

struct BackendRemoteLibraryServer final {
    QString server_id;
    QString display_name;
    bool embedded_previews_available = false;
    bool generated_proxies_available = false;
    bool originals_available = false;
    bool private_preview_provider_available = false;
};

/// One remote photo projected from the client-local mirror. `preview_path`
/// always names a verified local proxy and never a server-native source path.
struct BackendRemoteLibraryPhoto final {
    QString server_id;
    QString remote_photo_id;
    QString remote_representation_id;
    QString title;
    std::uint64_t source_byte_len = 0;
    bool has_source_modified_at = false;
    std::int64_t source_modified_at_ms = 0;
    bool has_original_identity = false;
    QString original_digest_hex;
    std::uint32_t representation_count = 1;
    std::uint32_t source_location_count = 1;
    bool has_raw_representation = false;
    bool has_raster_representation = false;
    bool has_preview = false;
    QString preview_path;
    QString preview_role;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    QString preview_unavailable_reason;
    bool has_captured_at = false;
    std::int64_t captured_at_unix_seconds = 0;
    QString camera_make;
    QString camera_model;
    QString lens_make;
    QString lens_model;
    bool has_iso_speed = false;
    double iso_speed = 0.0;
    bool has_exposure_time = false;
    double exposure_time_seconds = 0.0;
    bool has_aperture = false;
    double aperture_f_number = 0.0;
    bool has_focal_length = false;
    double focal_length_mm = 0.0;
    bool has_raw_dimensions = false;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    BackendReviewDecisionFlag decision_flag = BackendReviewDecisionFlag::Unflagged;
    std::uint8_t decision_rating = 0;
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t review_updated_at_ms = 0;
    bool is_materialized = false;
    QString local_photo_id;
    QString local_representation_id;
    QString local_source_path;
};

struct BackendRemoteLibrarySnapshot final {
    bool has_server = false;
    BackendRemoteLibraryServer server;
    QVector<BackendRemoteLibraryPhoto> photos;
};

struct BackendRemoteLibrarySyncResult final {
    BackendRemoteLibrarySnapshot snapshot;
    std::uint64_t page_count = 0;
    std::uint64_t photo_count = 0;
    std::uint64_t downloaded_previews = 0;
    std::uint64_t removed = 0;
};

struct BackendRemoteLibraryMaterialization final {
    QString local_photo_id;
    QString local_representation_id;
    QString local_source_path;
    QString title;
    bool reused_existing = false;
};
