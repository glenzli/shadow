#pragma once

#include "review_types.hpp"

#include <QString>
#include <QStringList>
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

struct BackendLibraryLivingPlaceRule final {
    QString locality_key;
    QString start_month;
    QString end_month;

    friend bool
    operator==(const BackendLibraryLivingPlaceRule&, const BackendLibraryLivingPlaceRule&) =
        default;
};

struct BackendLibraryPhotoFilter final {
    bool has_capture_start = false;
    std::int64_t capture_start_unix_seconds = 0;
    bool has_capture_end = false;
    std::int64_t capture_end_unix_seconds = 0;
    QString capture_month;
    bool has_chinese_lunar_month = false;
    std::uint8_t chinese_lunar_month = 0;
    bool has_chinese_lunar_day = false;
    std::uint8_t chinese_lunar_day = 0;
    bool has_chinese_lunar_is_leap_month = false;
    bool chinese_lunar_is_leap_month = false;
    QString camera_key;
    QString lens_key;
    QString country_key;
    QString locality_key;
    QVector<BackendLibraryLivingPlaceRule> living_place_rules;
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
    QStringList keyword_ids_all;
    QStringList excluded_keyword_ids_any;
};

/// Indexed, photo-first Library aggregation dimensions. A source directory is
/// deliberately absent: folders discover photos but never own the Library.
enum class BackendLibraryFacetKind : std::uint8_t {
    CaptureMonth,
    Camera,
    Lens,
    Country,
    City,
};

/// Exact coordinate work discovered by the Catalog. The provider may
/// prioritize coordinates shared by several current photos.
struct BackendLibraryPlaceResolutionCandidate final {
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    std::uint64_t photo_count = 0;
};

/// Provider-neutral structured reverse-geocoding output. Stable facet keys
/// and the final write time are deliberately not caller-controlled.
struct BackendLibraryPlaceResolutionResult final {
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    QString country_code;
    QString country_name;
    QString administrative_area;
    QString locality;
    QString display_name;
    QString provider_id;
    QString provider_version;
    QString locale;
};

enum class BackendRecordLibraryPlaceResolutionStatus : std::uint8_t {
    Recorded,
    CoordinatesNoLongerUsed,
};

enum class BackendLibraryPhotoOrder : std::uint8_t {
    CaptureTimeDescending,
    CaptureTimeAscending,
    FileNameAscending,
    FileNameDescending,
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

/// One node in the user-owned Library keyword taxonomy. `depth` is a
/// presentation projection; `parent_id` remains the durable relationship.
struct BackendLibraryKeyword final {
    QString id;
    QString parent_id;
    QString name;
    std::uint16_t depth = 0;
    std::uint64_t subtree_photo_count = 0;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
};

enum class BackendLibraryKeywordOrigin : std::uint8_t {
    Manual,
    Imported,
    AiAccepted,
};

struct BackendLibraryPhotoKeyword final {
    BackendLibraryKeyword keyword;
    BackendLibraryKeywordOrigin origin = BackendLibraryKeywordOrigin::Manual;
    QString source_label;
    bool has_confidence = false;
    std::uint16_t confidence_milli = 0;
    std::int64_t assigned_at_ms = 0;
};

struct BackendLibraryKeywordMutationReceipt final {
    QString keyword_id;
    std::uint64_t requested_photo_count = 0;
    std::uint64_t changed_photo_count = 0;
};

struct BackendLibraryKeywordDeletionReceipt final {
    std::uint64_t deleted_keyword_count = 0;
    std::uint64_t deleted_assignment_count = 0;
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

/// Keyset cursor for one explicitly ordered Library page. `photo_id` is the
/// stable tie-breaker; exactly one sort value family is present.
struct BackendLibraryPhotoCursor final {
    QString photo_id;
    bool has_capture_time = false;
    std::int64_t captured_at_unix_seconds = 0;
    QString file_name;
};

/// A bounded photo-first Library result. Exact count is deliberately separate
/// so virtualized scrolling never pays for an unbounded count query.
struct BackendLibraryPhotoPage final {
    QVector<BackendReviewItem> items;
    bool has_more = false;
    BackendLibraryPhotoCursor next_cursor;
};

/// Provider-independent map request. A west bound greater than east denotes
/// an antimeridian-crossing viewport.
struct BackendLibraryMapViewport final {
    std::int32_t south_latitude_e7 = 0;
    std::int32_t west_longitude_e7 = 0;
    std::int32_t north_latitude_e7 = 0;
    std::int32_t east_longitude_e7 = 0;
};

struct BackendLibraryMapGrid final {
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
};

struct BackendLibraryMapCluster final {
    std::uint16_t cell_x = 0;
    std::uint16_t cell_y = 0;
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    std::uint64_t photo_count = 0;
    QString photo_id;
    QString representation_id;
    QString title;
    QString source_path;
};

struct BackendLibraryMapSnapshot final {
    QVector<BackendLibraryMapCluster> clusters;
    std::uint64_t photo_count = 0;
};

struct BackendPhotoLibraryState final {
    QString photo_id;
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t updated_at_ms = 0;
};

struct BackendLibraryMetadataState final {
    QString photo_id;
    bool has_observed_capture_time = false;
    std::int64_t observed_captured_at_unix_seconds = 0;
    bool has_effective_capture_time = false;
    std::int64_t effective_captured_at_unix_seconds = 0;
    QString capture_time_override_mode = QStringLiteral("inherit");
    QString capture_time_override_origin;
    QString capture_time_source_label;
    bool has_observed_coordinates = false;
    std::int32_t observed_latitude_e7 = 0;
    std::int32_t observed_longitude_e7 = 0;
    bool has_effective_coordinates = false;
    std::int32_t effective_latitude_e7 = 0;
    std::int32_t effective_longitude_e7 = 0;
    QString effective_place_name;
    QString coordinates_override_mode = QStringLiteral("inherit");
    QString coordinates_override_origin;
    QString coordinates_source_label;
};

struct BackendGpxMatchProposal final {
    QString photo_id;
    std::int64_t captured_at_unix_seconds = 0;
    std::int64_t matched_at_unix_seconds = 0;
    std::uint32_t nearest_track_delta_seconds = 0;
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
};

struct BackendGpxImportPreview final {
    QString preview_id;
    QString source_path;
    QString source_digest_hex;
    std::uint32_t requested_photo_count = 0;
    std::uint32_t matched_photo_count = 0;
    std::uint32_t unmatched_photo_count = 0;
    QVector<BackendGpxMatchProposal> proposal_sample;
};

struct BackendCaptureTimeBatchProposal final {
    QString photo_id;
    bool has_before_capture_time = false;
    std::int64_t before_captured_at_unix_seconds = 0;
    bool has_after_capture_time = false;
    std::int64_t after_captured_at_unix_seconds = 0;
};

struct BackendCaptureTimeBatchPreview final {
    QString preview_id;
    QString mode;
    std::int64_t offset_seconds = 0;
    std::uint32_t requested_photo_count = 0;
    std::uint32_t applicable_photo_count = 0;
    std::uint32_t skipped_photo_count = 0;
    QVector<BackendCaptureTimeBatchProposal> proposal_sample;
};

struct BackendLibraryMetadataBatchReceipt final {
    std::uint32_t requested_photo_count = 0;
    std::uint32_t applied_photo_count = 0;
};
