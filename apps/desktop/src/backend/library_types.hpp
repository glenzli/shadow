#pragma once

#include "review_types.hpp"

#include <QString>
#include <QVector>

#include <cstdint>

// Photo-first Library filters, facets, albums, source-health evidence, and paging.
/// Explicit, photo-first Library facets. Empty text fields mean "any";
/// `has_*` booleans make a genuine zero/false constraint distinguishable from
/// an absent one. This DTO intentionally contains no directory/path cursor.
enum class BackendLibraryFlagFilter : std::uint8_t {
    Any,
    Unflagged,
    Picked,
    Rejected,
};

struct BackendLibraryPhotoFilter final {
    bool has_capture_start = false;
    std::int64_t capture_start_unix_seconds = 0;
    bool has_capture_end = false;
    std::int64_t capture_end_unix_seconds = 0;
    QString capture_month;
    QString camera_key;
    QString lens_key;
    bool has_aperture_minimum = false;
    std::uint32_t aperture_minimum_milli = 0;
    bool has_aperture_maximum = false;
    std::uint32_t aperture_maximum_milli = 0;
    bool has_liked = false;
    bool liked = false;
    QString color_label;
    BackendLibraryFlagFilter flag = BackendLibraryFlagFilter::Any;
    bool has_minimum_rating = false;
    std::uint8_t minimum_rating = 0;
    bool has_development_edits = false;
    bool development_edits = false;
    QString album_id;
};

/// Indexed, photo-first Library aggregation dimensions. A source directory is
/// deliberately absent: folders discover photos but never own the Library.
enum class BackendLibraryFacetKind : std::uint8_t {
    CaptureMonth,
    Camera,
    Lens,
};

struct BackendLibraryFacetCursor final {
    std::uint64_t photo_count = 0;
    QString key;
};

struct BackendLibraryFacet final {
    QString key;
    QString label;
    std::uint64_t photo_count = 0;
};

struct BackendLibraryFacetPage final {
    QVector<BackendLibraryFacet> items;
    bool has_more = false;
    BackendLibraryFacetCursor next_cursor;
};

/// Durable user organization in the local Library. Manual albums carry
/// explicit memberships; smart albums replay a frozen photo-first filter.
enum class BackendLibraryAlbumKind : std::uint8_t {
    Manual,
    Smart,
};

struct BackendLibraryAlbum final {
    QString id;
    BackendLibraryAlbumKind kind = BackendLibraryAlbumKind::Manual;
    QString name;
    BackendLibraryPhotoFilter query_filter;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
};

/// Read-only evidence from the most recent completed scan of a configured
/// Library source. It must never be interpreted as a global offline verdict
/// or an instruction to reattach a source automatically.
struct BackendLibrarySourceHealth final {
    QString source_id;
    QString source_display_path;
    bool source_enabled = false;
    bool has_latest_completed_scan = false;
    QString scan_session_id;
    std::int64_t scan_completed_at_ms = 0;
    std::uint64_t known_locations = 0;
    std::uint64_t seen_locations = 0;
    std::uint64_t not_seen_locations = 0;
};

/// One original location absent from a particular completed source scan. It is
/// scan-scoped review evidence only and never grants a caller reattach rights.
struct BackendMissingSourceLocation final {
    QString location_id;
    QString photo_id;
    QString title;
    QString source_display_path;
    bool has_captured_at = false;
    std::int64_t captured_at_unix_seconds = 0;
    QString camera_key;
    std::int64_t last_seen_at_ms = 0;
};

struct BackendMissingSourceLocationPage final {
    bool has_scan = false;
    QVector<BackendMissingSourceLocation> items;
    bool has_more = false;
    QString next_location_id;
};

/// Receipt for an explicit source reattach that passed complete identity
/// verification. It adds a source location to an existing photo.
struct BackendVerifiedSourceRelinkReceipt final {
    QString photo_id;
    QString representation_id;
    QString location_id;
    QString display_path;
};

/// Keyset cursor for capture-time-descending Library pages. `photo_id` is the
/// stable tie-breaker, so relinking/renaming a source never invalidates it.
struct BackendLibraryPhotoCursor final {
    QString photo_id;
    bool has_capture_time = false;
    std::int64_t captured_at_unix_seconds = 0;
};

/// A bounded photo-first Library result. Exact count is deliberately separate
/// so virtualized scrolling never pays for an unbounded count query.
struct BackendLibraryPhotoPage final {
    QVector<BackendReviewItem> items;
    bool has_more = false;
    BackendLibraryPhotoCursor next_cursor;
};

struct BackendPhotoLibraryState final {
    QString photo_id;
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t updated_at_ms = 0;
};
