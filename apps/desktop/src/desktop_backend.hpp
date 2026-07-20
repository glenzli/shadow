#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <compare>
#include <memory>

struct BackendScanReport final {
    QString folder_path;
    std::uint64_t files_seen = 0;
    std::uint64_t supported_files = 0;
    std::uint64_t decode_queued = 0;
    std::uint64_t issue_count = 0;
};

struct BackendReviewItem final {
    QString photo_id;
    QString representation_id;
    QString title;
    QString source_path;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
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

struct BackendReviewPage final {
    QVector<BackendReviewItem> items;
    QString next_cursor_path;
    QString next_cursor_representation_id;
    std::uint64_t total_items = 0;
    bool has_more = false;
};

struct BackendBasicEditParameters final {
    double exposure_stops = 0.0;
    double contrast_factor = 1.0;
    double red_channel_gain = 1.0;
    double green_channel_gain = 1.0;
    double blue_channel_gain = 1.0;
    double saturation_factor = 1.0;

    auto operator<=>(const BackendBasicEditParameters&) const = default;
};

struct BackendToneCurvePoint final {
    double x = 0.0;
    double y = 0.0;

    auto operator<=>(const BackendToneCurvePoint&) const = default;
};

struct BackendEditSettings final {
    BackendBasicEditParameters basic;
    bool has_tone_curve = false;
    QVector<BackendToneCurvePoint> tone_curve_points;

    bool operator==(const BackendEditSettings&) const = default;
};

struct BackendEditVersion final {
    QString commit_id;
    QString name;
    std::int64_t created_at_ms = 0;
    QVector<QString> parent_commit_ids;
    bool is_working = false;
    bool is_root = false;
    bool recipe_schema_changed = false;
    std::uint32_t layers_added = 0;
    std::uint32_t layers_removed = 0;
    std::uint32_t layers_moved = 0;
    std::uint32_t layers_modified = 0;
    std::uint32_t nodes_added = 0;
    std::uint32_t nodes_removed = 0;
    std::uint32_t nodes_modified = 0;
    std::uint32_t node_parameter_blocks_changed = 0;
    QVector<QString> changed_basic_parameters;
    std::uint32_t changed_basic_parameter_count = 0;
    bool has_other_changes = false;
};

struct BackendPhotoEditState final {
    QString photo_id;
    QString source_path;
    QString working_commit_id;
    QString recipe_id;
    BackendEditSettings settings;
    QVector<BackendEditVersion> versions;
    bool has_working_version = false;
};

struct BackendEditedPreview final {
    QByteArray bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class DesktopBackend final {
public:
    DesktopBackend(const QString& catalog_path, const QString& cache_root);
    ~DesktopBackend();

    DesktopBackend(const DesktopBackend&) = delete;
    DesktopBackend& operator=(const DesktopBackend&) = delete;

    [[nodiscard]] BackendScanReport scanFolder(const QString& folder_path) const;
    [[nodiscard]] BackendReviewPage reviewPage(
        const QString& cursor_path,
        const QString& cursor_representation_id,
        std::uint32_t limit
    ) const;
    [[nodiscard]] QByteArray loadReviewVisual(const QString& representation_id) const;
    [[nodiscard]] BackendPhotoEditState photoEditState(
        const QString& photo_id,
        const QString& source_path
    ) const;
    [[nodiscard]] BackendEditedPreview renderEditPreview(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendEditSettings& settings,
        std::uint32_t max_edge,
        std::uint8_t jpeg_quality,
        bool use_working_recipe
    ) const;
    [[nodiscard]] BackendPhotoEditState saveEditVersion(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendEditSettings& settings,
        const QString& version_name
    ) const;
    [[nodiscard]] BackendPhotoEditState checkoutEditVersion(
        const QString& photo_id,
        const QString& source_path,
        const QString& commit_id
    ) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
