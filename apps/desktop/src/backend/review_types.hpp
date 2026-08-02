#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>

// Review gallery/inspection projections and explicit human decision contracts.
enum class BackendReviewDecisionFlag : std::uint8_t {
    Unflagged,
    Picked,
    Rejected,
};

struct BackendReviewItem final {
    QString photo_id;
    QString representation_id;
    QString location_id;
    QString visual_handle;
    std::uint64_t decision_head_sequence = 0;
    BackendReviewDecisionFlag decision_flag = BackendReviewDecisionFlag::Unflagged;
    std::uint8_t decision_rating = 0;
    /// Mutable Catalog Library organization state. It is independent from
    /// picked/rejected and stars, which remain append-only Review decisions.
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t library_state_updated_at_ms = 0;
    bool has_development_edits = false;
    QString title;
    QString source_path;
    bool source_available = true;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
    bool has_metadata = false;
    QString camera_make;
    QString camera_model;
    QString lens_make;
    QString lens_model;
    std::int64_t captured_at_unix_seconds = 0;
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    double focal_length_35mm = 0.0;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    std::uint32_t sensor_bits = 0;
    QString cfa_pattern;
    QString dng_version;
    bool has_technical_observation = false;
    std::uint32_t technical_input_width = 0;
    std::uint32_t technical_input_height = 0;
    QString technical_preprocessing_version;
    QString technical_implementation_version;
    double mean_luma = 0.0;
    double p01_luma = 0.0;
    double p50_luma = 0.0;
    double p99_luma = 0.0;
    double near_black_fraction = 0.0;
    double near_white_fraction = 0.0;
    double laplacian_variance = 0.0;
    double edge_energy = 0.0;
};

/// Low-frequency details for one exact selected photo representation.
///
/// This stays independent from virtualized gallery rows: selection identity,
/// not delegate lifetime or RAW preference, determines the returned record.
struct BackendPhotoInspection final {
    bool available = false;
    QString photo_id;
    QString representation_id;
    QString source_path;
    std::uint64_t source_byte_len = 0;
    bool has_source_modified_at = false;
    std::int64_t source_modified_at_ms = 0;
    bool has_metadata = false;
    QString camera_make;
    QString camera_model;
    QString lens_make;
    QString lens_model;
    bool has_captured_at = false;
    std::int64_t captured_at_unix_seconds = 0;
    bool has_coordinates = false;
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    QString place_name;
    QString resolved_place_name;
    bool has_iso_speed = false;
    double iso_speed = 0.0;
    bool has_exposure_time = false;
    double exposure_time_seconds = 0.0;
    bool has_aperture = false;
    double aperture_f_number = 0.0;
    bool has_focal_length = false;
    double focal_length_mm = 0.0;
    bool has_focal_length_35mm = false;
    double focal_length_35mm = 0.0;
    bool has_raw_dimensions = false;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    bool has_sensor_bits = false;
    std::uint32_t sensor_bits = 0;
    QString cfa_pattern;
    QString dng_version;
    bool has_focus_observation = false;
    std::uint32_t focus_observation_schema_version = 0;
    QString focus_observation_source;
    double focus_observation_center_x = 0.0;
    double focus_observation_center_y = 0.0;
    double focus_observation_width = 0.0;
    double focus_observation_height = 0.0;
    bool focus_observation_confirmed = false;
    double focus_observation_confidence = 0.0;
    bool has_technical_observation = false;
    std::uint32_t technical_input_width = 0;
    std::uint32_t technical_input_height = 0;
    QString technical_preprocessing_version;
    QString technical_implementation_version;
    double mean_luma = 0.0;
    double p01_luma = 0.0;
    double p50_luma = 0.0;
    double p99_luma = 0.0;
    double near_black_fraction = 0.0;
    double near_white_fraction = 0.0;
    double laplacian_variance = 0.0;
    double edge_energy = 0.0;
};

struct BackendReviewVisual final {
    QByteArray bytes;
    bool requires_frame_receipt = false;
};

struct BackendReviewComparisonPresentation final {
    QString presentation_id;
    QString left_request_ticket;
    QString right_request_ticket;
};

struct BackendReviewDecisionState final {
    QString photo_id;
    std::uint64_t head_sequence = 0;
    BackendReviewDecisionFlag flag = BackendReviewDecisionFlag::Unflagged;
    std::uint8_t rating = 0;

    bool operator==(const BackendReviewDecisionState&) const = default;
};

struct BackendReviewDecisionMutationReceipt final {
    QString event_id;
    std::uint64_t sequence = 0;
    std::int64_t occurred_at_ms = 0;
    BackendReviewDecisionState before;
    BackendReviewDecisionState after;
};

enum class BackendPairwiseOutcome : std::uint8_t {
    LeftPreferred,
    RightPreferred,
    KeepBoth,
    KeepNeither,
    CannotCompare,
};

struct BackendFeedbackReceipt final {
    QString event_id;
    std::uint64_t sequence = 0;
    std::int64_t occurred_at_ms = 0;
};

struct BackendForgetReceipt final {
    QString fact_id;
    QString target_event_id;
    std::uint64_t sequence = 0;
    std::int64_t occurred_at_ms = 0;
};
