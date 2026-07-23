#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <QtConcurrent>

#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QImage>
#include <QFileInfo>

#include <QSize>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

// This is Shadow's resident editing proxy, not the full-resolution detail
// source. 768px is too aggressive for modern high-resolution RAWs: Bayer
// phase-preserving downsampling can turn a Z9 frame into a ~690px proxy.
// 1536px is still practical for interactive grading while preserving enough
// texture, edges and colour detail for a useful editing view.
constexpr std::uint32_t EDIT_PREVIEW_EDGE = 1'536;
// Interactive grading needs more headroom than gallery thumbnails. The image core also uses
// 4:4:4 JPEG sampling for these proxies so color-slider feedback does not add chroma blocks.
constexpr std::uint8_t EDIT_PREVIEW_QUALITY = 90;
// Gesture and settled previews deliberately share the same prepared source.
// Changing its edge would make the provider prepare a second RAW proxy on the
// first slider movement, which defeats the latency improvement. We vary only
// JPEG quality while dragging, then re-encode the already-cached linear proxy
// at the settled quality when the gesture ends.
constexpr std::uint32_t EDIT_INTERACTIVE_PREVIEW_EDGE = EDIT_PREVIEW_EDGE;
constexpr std::uint8_t EDIT_INTERACTIVE_PREVIEW_QUALITY = 84;
constexpr int EDIT_PREVIEW_THROTTLE_MS = 16;
constexpr int EDIT_AUTOSAVE_DEBOUNCE_MS = 700;
constexpr std::uint32_t EDIT_DETAIL_TILE_SIDE = 512;
constexpr std::uint32_t EDIT_LARGE_DETAIL_TILE_SIDE = 1'024;
constexpr int EDIT_DETAIL_DEBOUNCE_MS = 70;
constexpr int EDIT_DETAIL_WARMUP_IDLE_MS = 650;
constexpr std::uint64_t EDIT_DETAIL_MAX_PRESENTATION_BYTES = 96U * 1'024U * 1'024U;
constexpr qsizetype EDIT_HISTOGRAM_BIN_COUNT = 256;
constexpr int MAX_POINT_COLOR_COUNT = 16;

[[nodiscard]] bool raw_development_unavailable(const QString& error) noexcept {
    return error.startsWith(QStringLiteral("RAW development is unavailable:"));
}

[[nodiscard]] bool incompatible_development_recipe(const QString& error) noexcept {
    return error.startsWith(QStringLiteral("incompatible development Recipe:"));
}

[[nodiscard]] int point_color_count(const BackendFineEditParameters& fine) noexcept {
    return (fine.color_range_enabled || !fine.additional_point_colors.isEmpty() ? 1 : 0)
        + static_cast<int>(fine.additional_point_colors.size());
}

[[nodiscard]] BackendPointColorRange point_color_at(
    const BackendFineEditParameters& fine,
    const int index
) {
    if (index == 0 && point_color_count(fine) > 0) {
        return {
            .enabled = fine.color_range_enabled,
            .center_degrees = fine.color_range_center,
            .width_degrees = fine.color_range_width,
            .softness = fine.color_range_softness,
            .hue_shift_degrees = fine.color_range_hue,
            .saturation = fine.color_range_saturation,
            .lightness = fine.color_range_lightness,
        };
    }
    if (index > 0 && index - 1 < fine.additional_point_colors.size()) {
        return fine.additional_point_colors.at(index - 1);
    }
    throw std::out_of_range("invalid Point Color index");
}

void set_point_color_at(
    BackendFineEditParameters& fine,
    const int index,
    const BackendPointColorRange& range
) {
    if (index == 0) {
        fine.color_range_enabled = range.enabled;
        fine.color_range_center = range.center_degrees;
        fine.color_range_width = range.width_degrees;
        fine.color_range_softness = range.softness;
        fine.color_range_hue = range.hue_shift_degrees;
        fine.color_range_saturation = range.saturation;
        fine.color_range_lightness = range.lightness;
        return;
    }
    if (index > 0 && index - 1 < fine.additional_point_colors.size()) {
        fine.additional_point_colors[index - 1] = range;
        return;
    }
    throw std::out_of_range("invalid Point Color index");
}

[[nodiscard]] double srgb_to_linear(const double value) noexcept {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double sampled_oklch_hue(const QColor& color) noexcept {
    const double red = srgb_to_linear(color.redF());
    const double green = srgb_to_linear(color.greenF());
    const double blue = srgb_to_linear(color.blueF());
    const double l = std::cbrt(0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
    const double m = std::cbrt(0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
    const double s = std::cbrt(0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    double hue = std::atan2(b, a) * 180.0 / std::numbers::pi;
    if (hue < 0.0) {
        hue += 360.0;
    }
    return hue;
}

[[nodiscard]] bool is_neutral_oklab_lightness_curve(
    const QVector<double>& points
) noexcept {
    return points.isEmpty()
        || (points.size() == 4 && points[0] == 0.0 && points[1] == 0.0
            && points[2] == 1.0 && points[3] == 1.0);
}

void ensure_oklab_lightness_curve(BackendFineEditParameters& fine) {
    if (fine.oklab_lightness_curve_points.isEmpty()) {
        fine.oklab_lightness_curve_points = {0.0, 0.0, 1.0, 1.0};
    }
}

void clear_neutral_oklab_lightness_curve(BackendFineEditParameters& fine) {
    if (is_neutral_oklab_lightness_curve(fine.oklab_lightness_curve_points)) {
        fine.oklab_lightness_curve_points.clear();
    }
}

[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"EditController", source, arguments};
}

[[nodiscard]] bool bounded_profile_int(
    const QVariantMap& profile,
    const QString& key,
    const int minimum,
    const int maximum,
    int* const output
) {
    bool converted = false;
    const int value = profile.value(key).toInt(&converted);
    if (!converted || value < minimum || value > maximum) {
        return false;
    }
    *output = value;
    return true;
}

[[nodiscard]] QVariantList histogram_counts(
    const QVector<std::uint64_t>& counts
) {
    QVariantList result;
    result.reserve(counts.size());
    for (const auto count : counts) {
        result.push_back(QVariant::fromValue<qulonglong>(count));
    }
    return result;
}

[[nodiscard]] QVariantMap empty_histogram() {
    return {
        {QStringLiteral("valid"), false},
        {QStringLiteral("updating"), false},
        {QStringLiteral("stale"), false},
        {QStringLiteral("generation"), QVariant::fromValue<qulonglong>(0)},
        {QStringLiteral("targetGeneration"), QVariant::fromValue<qulonglong>(0)},
    };
}

[[nodiscard]] QVariantMap optics_receipt_map(const BackendOpticsReceipt& receipt) {
    return {
        {QStringLiteral("valid"), !receipt.status.isEmpty()},
        {QStringLiteral("status"), receipt.status},
        {QStringLiteral("providerId"), receipt.provider_id},
        {QStringLiteral("providerVersion"), receipt.provider_version},
        {QStringLiteral("cameraProfile"), receipt.camera_profile},
        {QStringLiteral("lensProfile"), receipt.lens_profile},
        {QStringLiteral("distortionAvailable"), receipt.distortion_available},
        {QStringLiteral("tcaAvailable"), receipt.tca_available},
        {QStringLiteral("vignettingAvailable"), receipt.vignetting_available},
        {QStringLiteral("appliedDistortion"), receipt.applied_distortion},
        {QStringLiteral("appliedTca"), receipt.applied_tca},
        {QStringLiteral("appliedVignetting"), receipt.applied_vignetting},
        {QStringLiteral("vignettingUsedDistanceFallback"),
            receipt.vignetting_used_distance_fallback},
        {QStringLiteral("appliedScaling"), receipt.applied_scaling},
    };
}

[[nodiscard]] QVariantMap histogram_snapshot(
    const BackendEditPreviewAnalysis& analysis,
    const quint64 generation
) {
    if (analysis.width == 0 || analysis.height == 0 || analysis.pixel_count == 0
        || analysis.red.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.green.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.blue.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.luma.size() != EDIT_HISTOGRAM_BIN_COUNT
        || analysis.below_zero_samples.size() != 3
        || analysis.above_one_samples.size() != 3) {
        throw std::runtime_error("edit preview analysis violated the desktop contract");
    }
    const double pixel_count = static_cast<double>(analysis.pixel_count);
    return {
        {QStringLiteral("valid"), true},
        {QStringLiteral("updating"), false},
        {QStringLiteral("stale"), false},
        {QStringLiteral("generation"), QVariant::fromValue<qulonglong>(generation)},
        {QStringLiteral("targetGeneration"), QVariant::fromValue<qulonglong>(generation)},
        {QStringLiteral("version"), analysis.version},
        {QStringLiteral("width"), analysis.width},
        {QStringLiteral("height"), analysis.height},
        {
            QStringLiteral("pixelCount"),
            QVariant::fromValue<qulonglong>(analysis.pixel_count)
        },
        {QStringLiteral("red"), histogram_counts(analysis.red)},
        {QStringLiteral("green"), histogram_counts(analysis.green)},
        {QStringLiteral("blue"), histogram_counts(analysis.blue)},
        {QStringLiteral("luma"), histogram_counts(analysis.luma)},
        {QStringLiteral("belowZero"), histogram_counts(analysis.below_zero_samples)},
        {QStringLiteral("aboveOne"), histogram_counts(analysis.above_one_samples)},
        {
            QStringLiteral("shadowClippedPixels"),
            QVariant::fromValue<qulonglong>(analysis.shadow_clipped_pixels)
        },
        {
            QStringLiteral("highlightClippedPixels"),
            QVariant::fromValue<qulonglong>(analysis.highlight_clipped_pixels)
        },
        {
            QStringLiteral("shadowClippedFraction"),
            static_cast<double>(analysis.shadow_clipped_pixels) / pixel_count
        },
        {
            QStringLiteral("highlightClippedFraction"),
            static_cast<double>(analysis.highlight_clipped_pixels) / pixel_count
        },
        {QStringLiteral("approximate"), true},
        {QStringLiteral("scope"), QStringLiteral("complete-warm-proxy")},
        {QStringLiteral("clippingRule"), QStringLiteral("strict-pre-clamp")},
    };
}

[[nodiscard]] QVector<ToneCurvePoint> tone_curve_model_points(
    const BackendGradeNode* const grade_node
) {
    if (grade_node == nullptr) {
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    const auto& source = grade_node->fine.oklab_lightness_curve_points;
    if (source.isEmpty() || source.size() % 2 != 0) {
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    QVector<ToneCurvePoint> points;
    points.reserve(source.size() / 2);
    for (qsizetype index = 0; index < source.size(); index += 2) {
        points.push_back({.x = source[index], .y = source[index + 1]});
    }
    return points;
}

[[nodiscard]] QVector<double> backend_oklab_lightness_curve_points(
    const ToneCurvePointModel& model
) {
    const auto source = model.points();
    QVector<double> points;
    points.reserve(source.size() * 2);
    for (const auto& point : source) {
        points.push_back(point.x);
        points.push_back(point.y);
    }
    return points;
}

[[nodiscard]] QString tone_curve_gesture_key(const int index) {
    return QStringLiteral("perceptual_tone_curve/point/%1").arg(index);
}

[[nodiscard]] QString display_grade_node_label(const QString& stored_label) {
    constexpr auto context = "EditController";
    const QString numbered_copy_marker = QStringLiteral(" Copy ");
    const qsizetype numbered_copy_index = stored_label.lastIndexOf(numbered_copy_marker);
    if (numbered_copy_index > 0) {
        const QString suffix = stored_label.mid(
            numbered_copy_index + numbered_copy_marker.size()
        );
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            const QString base = stored_label.left(numbered_copy_index);
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "%1 Copy %2")
            ).arg(display_grade_node_label(base)).arg(number);
        }
    }
    const QString copy_suffix = QStringLiteral(" Copy");
    if (stored_label.endsWith(copy_suffix) && stored_label.size() > copy_suffix.size()) {
        const QString base = stored_label.left(stored_label.size() - copy_suffix.size());
        return QCoreApplication::translate(
                   context,
                   QT_TRANSLATE_NOOP("EditController", "%1 Copy")
        ).arg(display_grade_node_label(base));
    }
    if (stored_label.compare(QStringLiteral("Adjustments"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate(
            context,
            QT_TRANSLATE_NOOP("EditController", "Adjustments")
        );
    }
    const QString numbered_prefix = QStringLiteral("Adjustments ");
    if (stored_label.startsWith(numbered_prefix, Qt::CaseInsensitive)) {
        const QString suffix = stored_label.mid(numbered_prefix.size());
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "Adjustments %1")
            ).arg(number);
        }
    }
    return stored_label;
}

[[nodiscard]] bool grade_node_list_changed(
    const BackendGradeStack& before,
    const BackendGradeStack& after
) {
    if (before.grade_nodes.size() != after.grade_nodes.size()) {
        return true;
    }
    for (qsizetype index = 0; index < before.grade_nodes.size(); ++index) {
        const auto& left = before.grade_nodes.at(index);
        const auto& right = after.grade_nodes.at(index);
        if (left.grade_node_id != right.grade_node_id || left.label != right.label
            || left.enabled != right.enabled) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString history_grade_node_id(const std::string& key) {
    const QString value = QString::fromStdString(key);
    constexpr QLatin1StringView prefix("grade_node/");
    if (!value.startsWith(prefix)) {
        return {};
    }
    const qsizetype prefix_size = prefix.size();
    const qsizetype end = value.indexOf(QLatin1Char('/'), prefix_size);
    return end < 0
        ? value.mid(prefix_size)
        : value.mid(prefix_size, end - prefix_size);
}

[[nodiscard]] EditStateTaskResult load_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Open;
    try {
        result.state = backend->photoEditState(photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult reset_incompatible_recipe_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::ResetIncompatibleRecipe;
    try {
        result.state = backend->resetIncompatiblePhotoEditHistory(photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult save_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack grade_stack,
    const QString& version_name,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Save;
    try {
        result.state = backend->saveEditVersion(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            grade_stack,
            version_name
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult autosave_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack grade_stack,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Autosave;
    try {
        result.state = backend->autosaveWorkingEdit(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            grade_stack
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult load_version_draft_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::LoadDraft;
    try {
        result.state = backend->loadEditVersionDraft(
            photo_id,
            source_path,
            commit_id
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditPreviewTaskResult render_preview(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack grade_stack,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const EditPreviewGeneration generation
) {
    EditPreviewTaskResult result;
    result.generation = generation;
    try {
        result.preview = backend->renderEditPreview(
            photo_id,
            source_path,
            base_commit_id,
            grade_stack,
            max_edge,
            jpeg_quality,
            generation.kind == EditPreviewKind::Current
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] BackendEditedDetailViewport compose_detail_viewport(
    BackendEditedDetailViewport viewport
) {
    if (viewport.full_width == 0 || viewport.full_height == 0 || viewport.tiles.isEmpty()) {
        throw std::runtime_error("full detail returned no RGB8 tiles");
    }
    std::uint32_t left = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
    std::uint64_t tile_pixels = 0;
    for (qsizetype index = 0; index < viewport.tiles.size(); ++index) {
        const auto& tile = viewport.tiles.at(index);
        const std::uint64_t expected_stride = static_cast<std::uint64_t>(tile.width) * 3U;
        const std::uint64_t expected_bytes = expected_stride * tile.height;
        const std::uint64_t tile_right = static_cast<std::uint64_t>(tile.x) + tile.width;
        const std::uint64_t tile_bottom = static_cast<std::uint64_t>(tile.y) + tile.height;
        if (tile.width == 0 || tile.height == 0 || tile.row_stride_bytes != expected_stride
            || expected_bytes != static_cast<std::uint64_t>(tile.bytes.size())
            || tile_right > viewport.full_width || tile_bottom > viewport.full_height) {
            throw std::runtime_error("full detail returned an invalid RGB8 tile layout");
        }
        for (qsizetype prior_index = 0; prior_index < index; ++prior_index) {
            const auto& prior = viewport.tiles.at(prior_index);
            const bool overlaps = tile.x < prior.x + prior.width
                && prior.x < tile.x + tile.width && tile.y < prior.y + prior.height
                && prior.y < tile.y + tile.height;
            if (overlaps) {
                throw std::runtime_error("full detail returned overlapping RGB8 tiles");
            }
        }
        left = std::min(left, tile.x);
        top = std::min(top, tile.y);
        right = std::max(right, static_cast<std::uint32_t>(tile_right));
        bottom = std::max(bottom, static_cast<std::uint32_t>(tile_bottom));
        tile_pixels += static_cast<std::uint64_t>(tile.width) * tile.height;
    }

    const std::uint32_t presentation_width = right - left;
    const std::uint32_t presentation_height = bottom - top;
    const std::uint64_t presentation_pixels =
        static_cast<std::uint64_t>(presentation_width) * presentation_height;
    const std::uint64_t presentation_bytes = presentation_pixels * 3U;
    if (presentation_width == 0 || presentation_height == 0
        || tile_pixels != presentation_pixels
        || presentation_bytes
            > static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::runtime_error("full detail tiles do not cover one complete viewport");
    }
    if (presentation_bytes > EDIT_DETAIL_MAX_PRESENTATION_BYTES) {
        throw std::runtime_error("full detail viewport exceeds the 96 MiB RGB limit");
    }

    QByteArray composite;
    composite.resize(static_cast<qsizetype>(presentation_bytes));
    composite.fill('\0');
    for (const auto& tile : viewport.tiles) {
        for (std::uint32_t row = 0; row < tile.height; ++row) {
            const std::size_t destination_offset =
                (static_cast<std::size_t>(tile.y - top + row) * presentation_width
                 + (tile.x - left))
                * 3U;
            const std::size_t source_offset =
                static_cast<std::size_t>(row) * tile.row_stride_bytes;
            std::memcpy(
                composite.data() + destination_offset,
                tile.bytes.constData() + source_offset,
                static_cast<std::size_t>(tile.row_stride_bytes)
            );
        }
    }
    viewport.tiles = {{
        .bytes = std::move(composite),
        .x = left,
        .y = top,
        .width = presentation_width,
        .height = presentation_height,
        .row_stride_bytes = presentation_width * 3U,
    }};
    return viewport;
}

[[nodiscard]] EditDetailTaskResult render_detail(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack grade_stack,
    const std::uint64_t render_token,
    const double center_x,
    const double center_y,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height,
    const EditDetailGeneration generation
) {
    EditDetailTaskResult result;
    result.generation = generation;
    try {
        const std::uint32_t tile_side = std::max(viewport_width, viewport_height) > 4'096U
            ? EDIT_LARGE_DETAIL_TILE_SIDE
            : EDIT_DETAIL_TILE_SIDE;
        result.viewport = compose_detail_viewport(
            backend->renderEditDetailViewport(
                photo_id,
                source_path,
                base_commit_id,
                grade_stack,
                render_token,
                center_x,
                center_y,
                viewport_width,
                viewport_height,
                tile_side,
                true
            )
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditDetailWarmupTaskResult warm_detail_source(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack grade_stack,
    const std::uint64_t render_token,
    const quint64 photo_generation,
    const quint64 render_revision
) {
    EditDetailWarmupTaskResult result;
    result.photo_generation = photo_generation;
    result.render_revision = render_revision;
    try {
        // One native 512px tile is enough to force the provider-neutral
        // full-resolution source preparation and prime the center of the
        // bounded Recipe-tile cache. Do not compose or publish it: this is an
        // idle optimisation only, never a hidden viewport change.
        static_cast<void>(backend->renderEditDetailViewport(
            photo_id,
            source_path,
            base_commit_id,
            grade_stack,
            render_token,
            0.5,
            0.5,
            EDIT_DETAIL_TILE_SIDE,
            EDIT_DETAIL_TILE_SIDE,
            EDIT_DETAIL_TILE_SIDE,
            true
        ));
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

EditController::EditController(
    std::shared_ptr<DesktopBackend> backend,
    std::shared_ptr<EditPreviewStore> preview_store,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      preview_store_(std::move(preview_store)),
      versions_(this),
      tone_curve_points_(this) {
    histogram_ = empty_histogram();
    before_histogram_ = empty_histogram();
    preview_debounce_.setSingleShot(true);
    detail_debounce_.setSingleShot(true);
    detail_warmup_debounce_.setSingleShot(true);
    autosave_debounce_.setSingleShot(true);
    connect(
        &preview_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startPreviewRender
    );
    connect(
        &state_watcher_,
        &QFutureWatcher<EditStateTaskResult>::finished,
        this,
        &EditController::finishStateTask
    );
    connect(
        &preview_watcher_,
        &QFutureWatcher<EditPreviewTaskResult>::finished,
        this,
        &EditController::finishPreviewTask
    );
    connect(
        &detail_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startDetailRender
    );
    connect(
        &autosave_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startAutosave
    );
    connect(
        &detail_watcher_,
        &QFutureWatcher<EditDetailTaskResult>::finished,
        this,
        &EditController::finishDetailTask
    );
    connect(
        &detail_warmup_debounce_,
        &QTimer::timeout,
        this,
        &EditController::startDetailWarmup
    );
    connect(
        &detail_warmup_watcher_,
        &QFutureWatcher<EditDetailWarmupTaskResult>::finished,
        this,
        &EditController::finishDetailWarmupTask
    );
  if (auto *const application = QCoreApplication::instance()) {
    application->installEventFilter(this);
  }
}

EditController::~EditController() {
    preview_debounce_.stop();
    detail_debounce_.stop();
    detail_warmup_debounce_.stop();
    autosave_debounce_.stop();
    detail_render_token_ = backend_->beginEditDetailRequest();
    detail_warmup_token_ = detail_render_token_;
    state_watcher_.waitForFinished();
    preview_watcher_.waitForFinished();
    detail_watcher_.waitForFinished();
    detail_warmup_watcher_.waitForFinished();
}

bool EditController::active() const noexcept {
    return active_;
}

bool EditController::busy() const noexcept {
    return state_running_ || current_rendering_ || before_rendering_ || detail_rendering_;
}

bool EditController::stateBusy() const noexcept {
    return interactionLocked();
}

bool EditController::interactionLocked() const noexcept {
    // Working snapshots are intentionally non-blocking: the editor keeps a
    // revisioned in-memory draft and rebases it on the committed autosave
    // head when the transaction returns. Opening a photo, creating a named
    // Version, and loading a Version still replace controller state, so they
    // remain interaction-locking operations.
    return state_running_ && state_task_kind_ != EditStateTaskKind::Autosave;
}

bool EditController::rendering() const noexcept {
    return current_rendering_;
}

bool EditController::beforeRendering() const noexcept {
    return before_rendering_;
}

bool EditController::detailMode() const noexcept {
    return detail_mode_;
}

bool EditController::detailRendering() const noexcept {
    return detail_rendering_;
}

QString EditController::detailErrorText() const {
    return detail_error_message_.translated();
}

quint32 EditController::detailFullWidth() const noexcept {
    return detail_full_width_;
}

quint32 EditController::detailFullHeight() const noexcept {
    return detail_full_height_;
}

quint64 EditController::detailRetainedBytes() const noexcept {
    return detail_retained_bytes_;
}

QVariantList EditController::detailTiles() const {
    return detail_tiles_;
}

bool EditController::dirty() const noexcept {
    return dirty_;
}

bool EditController::autosavePending() const noexcept {
    return !autosaveFailed() && (autosave_requested_ || autosave_debounce_.isActive()
        || (state_running_ && state_task_kind_ == EditStateTaskKind::Autosave
            && state_watcher_.isRunning()));
}

bool EditController::autosaveFailed() const noexcept {
    return !autosave_error_message_.isEmpty();
}

QString EditController::autosaveErrorText() const {
    return autosave_error_message_.translated();
}

bool EditController::versionDraft() const noexcept {
    return version_draft_;
}

bool EditController::canUndo() const noexcept {
    return history_.canUndo();
}

bool EditController::canRedo() const noexcept {
    return history_.canRedo();
}

QString EditController::photoId() const {
    return photo_id_;
}

QString EditController::representationId() const {
    return representation_id_;
}

QString EditController::title() const {
    return title_;
}

QString EditController::sourcePath() const {
    return source_path_;
}

QString EditController::previewSource() const {
    return preview_source_;
}

QString EditController::provisionalPreviewSource() const {
    return provisional_preview_source_;
}

QString EditController::beforePreviewSource() const {
    return before_preview_source_;
}

QVariantMap EditController::histogram() const {
    return histogram_;
}

QVariantMap EditController::beforeHistogram() const {
    return before_histogram_;
}

QString EditController::beforeErrorText() const {
    return before_error_message_.translated();
}

bool EditController::recipeRecoveryRequired() const noexcept {
    return !recipe_recovery_message_.isEmpty();
}

QString EditController::recipeRecoveryErrorText() const {
    return recipe_recovery_message_.translated();
}

QString EditController::statusText() const {
    return status_message_.translated();
}

bool EditController::opticsEnabled() const noexcept { return grade_stack_.optics.enabled; }
bool EditController::opticsDistortionEnabled() const noexcept {
    return grade_stack_.optics.correct_distortion;
}
bool EditController::opticsTcaEnabled() const noexcept {
    return grade_stack_.optics.correct_tca;
}
bool EditController::opticsVignettingEnabled() const noexcept {
    return grade_stack_.optics.correct_vignetting;
}
bool EditController::opticsAutomaticScale() const noexcept {
    return grade_stack_.optics.automatic_scale;
}
int EditController::manualOpticsDistortion() const noexcept {
    return grade_stack_.optics.manual_distortion;
}
int EditController::manualOpticsTcaRedCyan() const noexcept {
    return grade_stack_.optics.manual_tca_red_cyan;
}
int EditController::manualOpticsTcaBlueYellow() const noexcept {
    return grade_stack_.optics.manual_tca_blue_yellow;
}
int EditController::manualOpticsVignettingAmount() const noexcept {
    return grade_stack_.optics.manual_vignetting_amount;
}
int EditController::manualOpticsVignettingMidpoint() const noexcept {
    return grade_stack_.optics.manual_vignetting_midpoint;
}
QVariantMap EditController::opticsReceipt() const { return optics_receipt_; }
bool EditController::opticsManualProfile() const noexcept {
    return !grade_stack_.optics.camera_profile_model.isEmpty()
        && !grade_stack_.optics.lens_profile_model.isEmpty();
}
QString EditController::opticsCameraProfile() const {
    return grade_stack_.optics.camera_profile_model;
}
QString EditController::opticsLensProfile() const {
    return grade_stack_.optics.lens_profile_model;
}

QVariantList EditController::gradeNodes() const {
    QVariantList result;
    result.reserve(grade_stack_.grade_nodes.size());
    for (qsizetype index = 0; index < grade_stack_.grade_nodes.size(); ++index) {
        const auto& grade_node = grade_stack_.grade_nodes.at(index);
        QVariantMap item;
        item.insert(QStringLiteral("gradeNodeId"), grade_node.grade_node_id);
        item.insert(
            QStringLiteral("label"),
            display_grade_node_label(grade_node.label)
        );
        item.insert(QStringLiteral("rawLabel"), grade_node.label);
        item.insert(QStringLiteral("enabled"), grade_node.enabled);
        item.insert(QStringLiteral("index"), static_cast<int>(index));
        result.push_back(item);
    }
    return result;
}

int EditController::selectedGradeNodeIndex() const noexcept {
    return selected_grade_node_index_;
}

QString EditController::selectedGradeNodeId() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->grade_node_id;
}

bool EditController::hasSelectedGradeNode() const noexcept {
    return selectedGradeNode() != nullptr;
}

bool EditController::canAddGradeNode() const noexcept {
    return active_ && !interactionLocked()
        && grade_stack_.grade_nodes.size() < GradeNodeStack::maximum_grade_node_count;
}

bool EditController::canDeleteGradeNode() const noexcept {
    return active_ && !interactionLocked() && hasSelectedGradeNode()
        && grade_stack_.grade_nodes.size() > GradeNodeStack::minimum_grade_node_count;
}

bool EditController::canMoveGradeNodeUp() const noexcept {
    return active_ && !interactionLocked() && selected_grade_node_index_ > 0;
}

bool EditController::canMoveGradeNodeDown() const noexcept {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    return active_ && !interactionLocked() && selected_grade_node_index_ >= 0
        && selected_grade_node_index_ + 1 < count;
}

bool EditController::gradeNodeEnabled() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr && grade_node->enabled;
}

double EditController::exposureStops() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 0.0 : grade_node->basic.exposure_stops;
}

double EditController::contrastFactor() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->basic.contrast_factor;
}

double EditController::whiteBalanceTemperature() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 0.0 : grade_node->basic.white_balance_temperature;
}

double EditController::whiteBalanceTint() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 0.0 : grade_node->basic.white_balance_tint;
}

double EditController::saturationFactor() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->basic.saturation_factor;
}

QString EditController::lutResourceId() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->fine.lut_resource_id;
}

QString EditController::lutTitle() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->fine.lut_title;
}

bool EditController::hasLut() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr && !grade_node->fine.lut_resource_id.isEmpty();
}

double EditController::lutIntensity() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->fine.lut_intensity;
}

quint64 EditController::parameterRevision() const noexcept {
    return parameter_revision_;
}

QVariantList EditController::pointColors() const {
    QVariantList result;
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return result;
    }
    const int count = point_color_count(grade_node->fine);
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        const auto range = point_color_at(grade_node->fine, index);
        const QColor swatch = QColor::fromHslF(
            static_cast<float>(std::fmod(range.center_degrees + 360.0, 360.0) / 360.0),
            0.72F,
            0.55F
        );
        result.push_back(QVariantMap{
            {QStringLiteral("index"), index},
            {QStringLiteral("enabled"), range.enabled},
            {QStringLiteral("hue"), range.center_degrees},
            {QStringLiteral("swatch"), swatch.name(QColor::HexRgb)},
        });
    }
    return result;
}

int EditController::selectedPointColorIndex() const noexcept {
    return selected_point_color_index_;
}

bool EditController::pointColorPickerActive() const noexcept {
    return point_color_picker_active_;
}

bool EditController::whiteBalancePickerActive() const noexcept {
    return white_balance_picker_active_;
}

double EditController::parameterValue(const QString& parameter_key) const {
    const auto* const grade_node = selectedGradeNode();
    const BackendFineEditParameters neutral;
    const BackendFineEditParameters& fine = grade_node == nullptr
        ? neutral
        : grade_node->fine;
    if (parameter_key == QStringLiteral("highlights")) {
        return fine.highlights;
    }
    if (parameter_key == QStringLiteral("shadows")) {
        return fine.shadows;
    }
    if (parameter_key == QStringLiteral("whites")) {
        return fine.whites;
    }
    if (parameter_key == QStringLiteral("blacks")) {
        return fine.blacks;
    }
    if (parameter_key == QStringLiteral("vibrance")) {
        return fine.vibrance;
    }
    if (parameter_key == QStringLiteral("lut_intensity")) {
        return fine.lut_intensity;
    }
    if (parameter_key == QStringLiteral("sharpen_amount")) {
        return fine.sharpen_amount;
    }
    if (parameter_key == QStringLiteral("sharpen_radius")) {
        return fine.sharpen_radius;
    }
    if (parameter_key == QStringLiteral("sharpen_threshold")) {
        return fine.sharpen_threshold;
    }
    if (parameter_key == QStringLiteral("sharpen_masking")) {
        return fine.sharpen_masking;
    }
    if (parameter_key == QStringLiteral("clarity")) return fine.clarity;
    if (parameter_key == QStringLiteral("texture")) return fine.texture;
    if (parameter_key == QStringLiteral("selective_color_lightness_protection")) {
        return fine.selective_color_lightness_protection;
    }
    if (parameter_key == QStringLiteral("denoise_luminance")) return fine.denoise_luminance;
    if (parameter_key == QStringLiteral("denoise_detail")) return fine.denoise_detail;
    if (parameter_key == QStringLiteral("denoise_color")) return fine.denoise_color;
    if (parameter_key == QStringLiteral("dehaze")) return fine.dehaze;
    if (parameter_key == QStringLiteral("defringe_purple_amount")) {
        return fine.defringe_purple_amount;
    }
    if (parameter_key == QStringLiteral("defringe_purple_hue_low")) {
        return fine.defringe_purple_hue_low;
    }
    if (parameter_key == QStringLiteral("defringe_purple_hue_high")) {
        return fine.defringe_purple_hue_high;
    }
    if (parameter_key == QStringLiteral("defringe_green_amount")) {
        return fine.defringe_green_amount;
    }
    if (parameter_key == QStringLiteral("defringe_green_hue_low")) {
        return fine.defringe_green_hue_low;
    }
    if (parameter_key == QStringLiteral("defringe_green_hue_high")) {
        return fine.defringe_green_hue_high;
    }
    if (parameter_key == QStringLiteral("shadows_hue")) return fine.shadows_hue;
    if (parameter_key == QStringLiteral("shadows_saturation")) return fine.shadows_saturation;
    if (parameter_key == QStringLiteral("shadows_luminance")) return fine.shadows_luminance;
    if (parameter_key == QStringLiteral("midtones_hue")) return fine.midtones_hue;
    if (parameter_key == QStringLiteral("midtones_saturation")) return fine.midtones_saturation;
    if (parameter_key == QStringLiteral("midtones_luminance")) return fine.midtones_luminance;
    if (parameter_key == QStringLiteral("highlights_hue")) return fine.highlights_hue;
    if (parameter_key == QStringLiteral("highlights_saturation")) return fine.highlights_saturation;
    if (parameter_key == QStringLiteral("highlights_luminance")) return fine.highlights_luminance;
    if (parameter_key == QStringLiteral("grading_blending")) return fine.grading_blending;
    if (parameter_key == QStringLiteral("grading_balance")) return fine.grading_balance;
    if (parameter_key == QStringLiteral("grain_amount")) return fine.grain_amount;
    if (parameter_key == QStringLiteral("grain_size")) return fine.grain_size;
    if (parameter_key == QStringLiteral("grain_roughness")) return fine.grain_roughness;
    if (parameter_key == QStringLiteral("vignette_amount")) return fine.vignette_amount;
    if (parameter_key == QStringLiteral("vignette_midpoint")) return fine.vignette_midpoint;
    if (parameter_key == QStringLiteral("vignette_roundness")) return fine.vignette_roundness;
    if (parameter_key == QStringLiteral("vignette_feather")) return fine.vignette_feather;
    if (parameter_key == QStringLiteral("vignette_highlights")) return fine.vignette_highlights;
    if (parameter_key.startsWith(QStringLiteral("color_range_"))) {
        if (selected_point_color_index_ < 0
            || selected_point_color_index_ >= point_color_count(fine)) {
            return parameter_key == QStringLiteral("color_range_width") ? 30.0
                : parameter_key == QStringLiteral("color_range_softness") ? 0.5 : 0.0;
        }
        const auto range = point_color_at(fine, selected_point_color_index_);
        if (parameter_key == QStringLiteral("color_range_enabled")) return range.enabled ? 1.0 : 0.0;
        if (parameter_key == QStringLiteral("color_range_center")) return range.center_degrees;
        if (parameter_key == QStringLiteral("color_range_width")) return range.width_degrees;
        if (parameter_key == QStringLiteral("color_range_softness")) return range.softness;
        if (parameter_key == QStringLiteral("color_range_hue")) return range.hue_shift_degrees;
        if (parameter_key == QStringLiteral("color_range_saturation")) return range.saturation;
        if (parameter_key == QStringLiteral("color_range_lightness")) return range.lightness;
    }
    return 0.0;
}

double EditController::colorMixerValue(
    const int band_index,
    const QString& component
) const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || band_index < 0
        || static_cast<std::size_t>(band_index) >= BACKEND_COLOR_MIXER_BAND_COUNT) {
        return 0.0;
    }
    const std::size_t index = static_cast<std::size_t>(band_index);
    if (component == QStringLiteral("hue")) {
        return grade_node->fine.mixer_hue[index];
    }
    if (component == QStringLiteral("saturation")) {
        return grade_node->fine.mixer_saturation[index];
    }
    if (component == QStringLiteral("lightness")) {
        return grade_node->fine.mixer_lightness[index];
    }
    return 0.0;
}

QAbstractItemModel* EditController::toneCurvePoints() noexcept {
    return &tone_curve_points_;
}

bool EditController::hasToneCurve() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr
        && !is_neutral_oklab_lightness_curve(grade_node->fine.oklab_lightness_curve_points);
}

bool EditController::toneCurveEditable() const noexcept {
    return tone_curve_points_.isEditable();
}

QAbstractItemModel* EditController::versions() noexcept {
    return &versions_;
}

void EditController::setGradeNodeEnabled(const bool enabled) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    const QString grade_node_id = grade_node->grade_node_id;
    grade_stack_.grade_nodes[selected_grade_node_index_].enabled = enabled;
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/enabled").arg(grade_node_id),
        before
    );
    emit gradeNodesChanged();
    emit gradeNodeEnabledChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(0);
  setStatusMessage(edit_message(
        enabled ? QT_TRANSLATE_NOOP("EditController", "Grade Node enabled")
                : QT_TRANSLATE_NOOP(
                    "EditController",
                    "Grade Node bypassed · settings preserved"))
    );
}

void EditController::setExposureStops(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.exposure_stops == value
        || !acceptParameter(value, -16.0, 16.0,
                       QT_TRANSLATE_NOOP("EditController", "Exposure"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.exposure_stops = value;
    parameterEdited(QStringLiteral("exposure"), before);
}

void EditController::setContrastFactor(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.contrast_factor == value
        || !acceptParameter(value, 0.0, 8.0,
                       QT_TRANSLATE_NOOP("EditController", "Contrast"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.contrast_factor = value;
    parameterEdited(QStringLiteral("contrast"), before);
}

void EditController::setWhiteBalanceTemperature(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.white_balance_temperature == value
        || !acceptParameter(value, -1.0, 1.0,
                       QT_TRANSLATE_NOOP("EditController", "Temperature"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.white_balance_temperature = value;
    parameterEdited(QStringLiteral("white_balance_temperature"), before);
}

void EditController::setWhiteBalanceTint(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.white_balance_tint == value
        || !acceptParameter(value, -1.0, 1.0,
                       QT_TRANSLATE_NOOP("EditController", "Tint"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.white_balance_tint = value;
    parameterEdited(QStringLiteral("white_balance_tint"), before);
}

void EditController::setSaturationFactor(const double value) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->basic.saturation_factor == value
        || !acceptParameter(value, 0.0, 8.0,
                       QT_TRANSLATE_NOOP("EditController", "Chroma"))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].basic.saturation_factor = value;
    parameterEdited(QStringLiteral("saturation"), before);
}

void EditController::setLutIntensity(const double value) {
    setParameterValue(QStringLiteral("lut_intensity"), value);
}

void EditController::setOpticsEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.enabled == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.enabled = enabled;
    opticsEdited(QStringLiteral("enabled"), before);
}

void EditController::setOpticsDistortionEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_distortion == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_distortion = enabled;
    opticsEdited(QStringLiteral("distortion"), before);
}

void EditController::setOpticsTcaEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_tca == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_tca = enabled;
    opticsEdited(QStringLiteral("tca"), before);
}

void EditController::setOpticsVignettingEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_vignetting == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_vignetting = enabled;
    opticsEdited(QStringLiteral("vignetting"), before);
}

void EditController::setOpticsAutomaticScale(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.automatic_scale == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.automatic_scale = enabled;
    opticsEdited(QStringLiteral("automatic_scale"), before);
}

void EditController::setManualOpticsDistortion(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_distortion == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_distortion = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_distortion"), before);
}

void EditController::setManualOpticsTcaRedCyan(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_tca_red_cyan == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_tca_red_cyan = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_tca_red_cyan"), before);
}

void EditController::setManualOpticsTcaBlueYellow(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_tca_blue_yellow == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_tca_blue_yellow = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_tca_blue_yellow"), before);
}

void EditController::setManualOpticsVignettingAmount(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_vignetting_amount == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_vignetting_amount = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_vignetting_amount"), before);
}

void EditController::setManualOpticsVignettingMidpoint(const int value) {
    if (!active_ || interactionLocked() || value < 0 || value > 100
        || grade_stack_.optics.manual_vignetting_midpoint == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_vignetting_midpoint = static_cast<std::uint8_t>(value);
    opticsEdited(QStringLiteral("manual_vignetting_midpoint"), before);
}

QVariantList EditController::opticsProfileCandidates() {
    if (!active_ || photo_id_.isEmpty() || source_path_.isEmpty()) return {};
    try {
        return backend_->opticsProfileCandidates(photo_id_, source_path_);
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController", "Could not read Lensfun profiles · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return {};
    }
}

void EditController::applyManualOpticsProfile(const QVariantMap& profile) {
    if (!active_ || interactionLocked()) {
        return;
    }
    int distortion = 0;
    int tca_red_cyan = 0;
    int tca_blue_yellow = 0;
    int vignetting_amount = 0;
    int vignetting_midpoint = 50;
    const bool valid = bounded_profile_int(
                           profile,
                           QStringLiteral("manualDistortion"),
                           -100,
                           100,
                           &distortion
                       )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualTcaRedCyan"),
            -100,
            100,
            &tca_red_cyan
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualTcaBlueYellow"),
            -100,
            100,
            &tca_blue_yellow
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualVignettingAmount"),
            -100,
            100,
            &vignetting_amount
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualVignettingMidpoint"),
            0,
            100,
            &vignetting_midpoint
        );
    if (!valid) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "This local optical profile is invalid")));
        return;
    }
    if (grade_stack_.optics.manual_distortion == distortion
        && grade_stack_.optics.manual_tca_red_cyan == tca_red_cyan
        && grade_stack_.optics.manual_tca_blue_yellow == tca_blue_yellow
        && grade_stack_.optics.manual_vignetting_amount == vignetting_amount
        && grade_stack_.optics.manual_vignetting_midpoint == vignetting_midpoint) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_distortion = static_cast<std::int16_t>(distortion);
    grade_stack_.optics.manual_tca_red_cyan = static_cast<std::int16_t>(tca_red_cyan);
    grade_stack_.optics.manual_tca_blue_yellow = static_cast<std::int16_t>(tca_blue_yellow);
    grade_stack_.optics.manual_vignetting_amount = static_cast<std::int16_t>(vignetting_amount);
    grade_stack_.optics.manual_vignetting_midpoint = static_cast<std::uint8_t>(vignetting_midpoint);
    const QString profile_id = profile.value(QStringLiteral("id")).toString();
    opticsEdited(
        QStringLiteral("manual_profile/%1").arg(profile_id.isEmpty()
            ? QStringLiteral("custom") : profile_id),
        before
    );
    const QString title = profile.value(QStringLiteral("title")).toString().trimmed();
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController", "Applied manual optical profile · %1"),
        {title.isEmpty() ? tr("Custom profile") : title}
    ));
}

void EditController::setManualOpticsProfile(
    const QString& camera_maker,
    const QString& camera_model,
    const QString& lens_maker,
    const QString& lens_model
) {
    if (!active_ || interactionLocked() || camera_model.trimmed().isEmpty()
        || lens_model.trimmed().isEmpty()) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.camera_profile_maker = camera_maker.trimmed();
    grade_stack_.optics.camera_profile_model = camera_model.trimmed();
    grade_stack_.optics.lens_profile_maker = lens_maker.trimmed();
    grade_stack_.optics.lens_profile_model = lens_model.trimmed();
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::clearManualOpticsProfile() {
    if (!active_ || interactionLocked() || !opticsManualProfile()) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.camera_profile_maker.clear();
    grade_stack_.optics.camera_profile_model.clear();
    grade_stack_.optics.lens_profile_maker.clear();
    grade_stack_.optics.lens_profile_model.clear();
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::setLutResource(
    const QString& resource_id,
    const QString& title,
    const QString& managed_path
) {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    const QFileInfo path(managed_path);
    const bool valid_id = resource_id.size() == 64
        && std::ranges::all_of(resource_id, [](const QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        });
    if (grade_node == nullptr || !valid_id || title.trimmed().isEmpty()
        || title.toUtf8().size() > 512 || !path.isAbsolute()
        || path.suffix() != QStringLiteral("cube")
        || path.completeBaseName() != resource_id) {
        return;
    }
    auto& fine = grade_node->fine;
    if (fine.lut_resource_id == resource_id && fine.lut_title == title
        && fine.lut_managed_path == managed_path) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    fine.lut_resource_id = resource_id;
    fine.lut_title = title;
    fine.lut_managed_path = managed_path;
    parameterEdited(QStringLiteral("lut/resource"), before);
}

void EditController::clearLut() {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->fine.lut_resource_id.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->fine.lut_resource_id.clear();
    grade_node->fine.lut_title.clear();
    grade_node->fine.lut_managed_path.clear();
    parameterEdited(QStringLiteral("lut/resource"), before);
}

void EditController::setColorGradingWheel(
    const QString& tonal_range,
    const double hue,
    const double saturation
) {
    const auto* const selected = selectedGradeNode();
    if (selected == nullptr
        || !acceptParameter(hue, 0.0, 360.0,
                            QT_TRANSLATE_NOOP("EditController", "Color grading hue"))
        || !acceptParameter(saturation, 0.0, 1.0,
                            QT_TRANSLATE_NOOP("EditController", "Color grading saturation"))) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    double* target_hue = nullptr;
    double* target_saturation = nullptr;
    if (tonal_range == QStringLiteral("shadows")) {
        target_hue = &fine.shadows_hue;
        target_saturation = &fine.shadows_saturation;
    } else if (tonal_range == QStringLiteral("midtones")) {
        target_hue = &fine.midtones_hue;
        target_saturation = &fine.midtones_saturation;
    } else if (tonal_range == QStringLiteral("highlights")) {
        target_hue = &fine.highlights_hue;
        target_saturation = &fine.highlights_saturation;
    } else {
        return;
    }
    if (*target_hue == hue && *target_saturation == saturation) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target_hue = hue;
    *target_saturation = saturation;
    parameterEdited(QStringLiteral("color_grading/%1/wheel").arg(tonal_range), before);
}

void EditController::setDefringeHueRange(
    const QString& family,
    const double lower_hue,
    const double upper_hue
) {
    const auto* const selected = selectedGradeNode();
    if (selected == nullptr || upper_hue - lower_hue < 10.0
        || !acceptParameter(lower_hue, 0.0, 360.0,
                            QT_TRANSLATE_NOOP("EditController", "Defringe hue range"))
        || !acceptParameter(upper_hue, 0.0, 360.0,
                            QT_TRANSLATE_NOOP("EditController", "Defringe hue range"))) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    double* target_low = nullptr;
    double* target_high = nullptr;
    if (family == QStringLiteral("purple")) {
        target_low = &fine.defringe_purple_hue_low;
        target_high = &fine.defringe_purple_hue_high;
    } else if (family == QStringLiteral("green")) {
        target_low = &fine.defringe_green_hue_low;
        target_high = &fine.defringe_green_hue_high;
    } else {
        return;
    }
    if (*target_low == lower_hue && *target_high == upper_hue) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target_low = lower_hue;
    *target_high = upper_hue;
    parameterEdited(QStringLiteral("optics/defringe/%1/hue_range").arg(family), before);
}

void EditController::setParameterValue(
    const QString& parameter_key,
    const double value
) {
    const auto* const selected = selectedGradeNode();
    if (selected == nullptr) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    if (parameter_key.startsWith(QStringLiteral("color_range_"))) {
        const int count = point_color_count(fine);
        if (selected_point_color_index_ < 0 || selected_point_color_index_ >= count) {
            return;
        }
        BackendPointColorRange range = point_color_at(fine, selected_point_color_index_);
        double minimum = -1.0;
        double maximum = 1.0;
        double current = 0.0;
        if (parameter_key == QStringLiteral("color_range_enabled")) {
            minimum = 0.0; maximum = 1.0; current = range.enabled ? 1.0 : 0.0;
        } else if (parameter_key == QStringLiteral("color_range_center")) {
            minimum = 0.0; maximum = 360.0; current = range.center_degrees;
        } else if (parameter_key == QStringLiteral("color_range_width")) {
            minimum = 1.0; maximum = 180.0; current = range.width_degrees;
        } else if (parameter_key == QStringLiteral("color_range_softness")) {
            minimum = 0.0; maximum = 1.0; current = range.softness;
        } else if (parameter_key == QStringLiteral("color_range_hue")) {
            minimum = -180.0; maximum = 180.0; current = range.hue_shift_degrees;
        } else if (parameter_key == QStringLiteral("color_range_saturation")) {
            current = range.saturation;
        } else if (parameter_key == QStringLiteral("color_range_lightness")) {
            current = range.lightness;
        } else {
            return;
        }
        if (current == value || !acceptParameter(
                value, minimum, maximum,
                QT_TRANSLATE_NOOP("EditController", "Point Color")
            )) {
            return;
        }
        const BackendGradeStack before = grade_stack_;
        if (parameter_key == QStringLiteral("color_range_enabled")) range.enabled = value >= 0.5;
        else if (parameter_key == QStringLiteral("color_range_center")) range.center_degrees = value;
        else if (parameter_key == QStringLiteral("color_range_width")) range.width_degrees = value;
        else if (parameter_key == QStringLiteral("color_range_softness")) range.softness = value;
        else if (parameter_key == QStringLiteral("color_range_hue")) range.hue_shift_degrees = value;
        else if (parameter_key == QStringLiteral("color_range_saturation")) range.saturation = value;
        else if (parameter_key == QStringLiteral("color_range_lightness")) range.lightness = value;
        set_point_color_at(fine, selected_point_color_index_, range);
        parameterEdited(
            QStringLiteral("point_color/%1/%2").arg(selected_point_color_index_).arg(parameter_key),
            before
        );
        return;
    }
    double minimum = -1.0;
    double maximum = 1.0;
    const char* label = QT_TRANSLATE_NOOP("EditController", "Adjustment");
    double* target = nullptr;
    if (parameter_key == QStringLiteral("highlights")) {
        target = &fine.highlights;
        label = QT_TRANSLATE_NOOP("EditController", "Highlights");
    } else if (parameter_key == QStringLiteral("shadows")) {
        target = &fine.shadows;
        label = QT_TRANSLATE_NOOP("EditController", "Shadows");
    } else if (parameter_key == QStringLiteral("whites")) {
        target = &fine.whites;
        label = QT_TRANSLATE_NOOP("EditController", "Whites");
    } else if (parameter_key == QStringLiteral("blacks")) {
        target = &fine.blacks;
        label = QT_TRANSLATE_NOOP("EditController", "Blacks");
    } else if (parameter_key == QStringLiteral("vibrance")) {
        target = &fine.vibrance;
        label = QT_TRANSLATE_NOOP("EditController", "Vibrance");
    } else if (parameter_key == QStringLiteral("lut_intensity")) {
        target = &fine.lut_intensity;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "LUT intensity");
    } else if (parameter_key == QStringLiteral("sharpen_amount")) {
        target = &fine.sharpen_amount;
        minimum = 0.0;
        maximum = 2.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening amount");
    } else if (parameter_key == QStringLiteral("sharpen_radius")) {
        target = &fine.sharpen_radius;
        minimum = 0.1;
        maximum = 5.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening radius");
    } else if (parameter_key == QStringLiteral("sharpen_threshold")) {
        target = &fine.sharpen_threshold;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening threshold");
    } else if (parameter_key == QStringLiteral("sharpen_masking")) {
        target = &fine.sharpen_masking;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Sharpening masking");
    } else if (parameter_key == QStringLiteral("clarity")) {
        target = &fine.clarity;
        label = QT_TRANSLATE_NOOP("EditController", "Perceptual clarity");
    } else if (parameter_key == QStringLiteral("texture")) {
        target = &fine.texture;
        label = QT_TRANSLATE_NOOP("EditController", "Perceptual texture");
    } else if (parameter_key == QStringLiteral("selective_color_lightness_protection")) {
        target = &fine.selective_color_lightness_protection;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Selective Color lightness protection");
    } else if (parameter_key == QStringLiteral("denoise_luminance")) {
        target = &fine.denoise_luminance; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Luminance noise reduction");
    } else if (parameter_key == QStringLiteral("denoise_detail")) {
        target = &fine.denoise_detail; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Noise reduction detail");
    } else if (parameter_key == QStringLiteral("denoise_color")) {
        target = &fine.denoise_color; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color noise reduction");
    } else if (parameter_key == QStringLiteral("dehaze")) {
        target = &fine.dehaze;
        label = QT_TRANSLATE_NOOP("EditController", "Dehaze");
    } else if (parameter_key == QStringLiteral("defringe_purple_amount")) {
        target = &fine.defringe_purple_amount; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Purple defringe amount");
    } else if (parameter_key == QStringLiteral("defringe_green_amount")) {
        target = &fine.defringe_green_amount; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Green defringe amount");
    } else if (parameter_key == QStringLiteral("shadows_hue")) {
        target = &fine.shadows_hue; minimum = 0.0; maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Shadow grading hue");
    } else if (parameter_key == QStringLiteral("shadows_saturation")) {
        target = &fine.shadows_saturation; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Shadow grading saturation");
    } else if (parameter_key == QStringLiteral("shadows_luminance")) {
        target = &fine.shadows_luminance;
        label = QT_TRANSLATE_NOOP("EditController", "Shadow grading luminance");
    } else if (parameter_key == QStringLiteral("midtones_hue")) {
        target = &fine.midtones_hue; minimum = 0.0; maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Midtone grading hue");
    } else if (parameter_key == QStringLiteral("midtones_saturation")) {
        target = &fine.midtones_saturation; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Midtone grading saturation");
    } else if (parameter_key == QStringLiteral("midtones_luminance")) {
        target = &fine.midtones_luminance;
        label = QT_TRANSLATE_NOOP("EditController", "Midtone grading luminance");
    } else if (parameter_key == QStringLiteral("highlights_hue")) {
        target = &fine.highlights_hue; minimum = 0.0; maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Highlight grading hue");
    } else if (parameter_key == QStringLiteral("highlights_saturation")) {
        target = &fine.highlights_saturation; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Highlight grading saturation");
    } else if (parameter_key == QStringLiteral("highlights_luminance")) {
        target = &fine.highlights_luminance;
        label = QT_TRANSLATE_NOOP("EditController", "Highlight grading luminance");
    } else if (parameter_key == QStringLiteral("grading_blending")) {
        target = &fine.grading_blending; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color grading blending");
    } else if (parameter_key == QStringLiteral("grading_balance")) {
        target = &fine.grading_balance;
        label = QT_TRANSLATE_NOOP("EditController", "Color grading balance");
    } else if (parameter_key == QStringLiteral("grain_amount")) {
        target = &fine.grain_amount; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Grain amount");
    } else if (parameter_key == QStringLiteral("grain_size")) {
        target = &fine.grain_size; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Grain size");
    } else if (parameter_key == QStringLiteral("grain_roughness")) {
        target = &fine.grain_roughness; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Grain roughness");
    } else if (parameter_key == QStringLiteral("vignette_amount")) {
        target = &fine.vignette_amount;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette amount");
    } else if (parameter_key == QStringLiteral("vignette_midpoint")) {
        target = &fine.vignette_midpoint; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette midpoint");
    } else if (parameter_key == QStringLiteral("vignette_roundness")) {
        target = &fine.vignette_roundness;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette roundness");
    } else if (parameter_key == QStringLiteral("vignette_feather")) {
        target = &fine.vignette_feather; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette feather");
    } else if (parameter_key == QStringLiteral("vignette_highlights")) {
        target = &fine.vignette_highlights; minimum = 0.0; maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Vignette highlights");
    } else if (parameter_key == QStringLiteral("color_range_center")) {
        target = &fine.color_range_center;
        minimum = 0.0;
        maximum = 360.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range hue");
    } else if (parameter_key == QStringLiteral("color_range_width")) {
        target = &fine.color_range_width;
        minimum = 1.0;
        maximum = 180.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range width");
    } else if (parameter_key == QStringLiteral("color_range_softness")) {
        target = &fine.color_range_softness;
        minimum = 0.0;
        maximum = 1.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range softness");
    } else if (parameter_key == QStringLiteral("color_range_hue")) {
        target = &fine.color_range_hue;
        minimum = -180.0;
        maximum = 180.0;
        label = QT_TRANSLATE_NOOP("EditController", "Color range hue shift");
    } else if (parameter_key == QStringLiteral("color_range_saturation")) {
        target = &fine.color_range_saturation;
        label = QT_TRANSLATE_NOOP("EditController", "Color range saturation");
    } else if (parameter_key == QStringLiteral("color_range_lightness")) {
        target = &fine.color_range_lightness;
        label = QT_TRANSLATE_NOOP("EditController", "Color range lightness");
    } else if (parameter_key == QStringLiteral("color_range_enabled")) {
        if (!acceptParameter(
                value,
                0.0,
                1.0,
                QT_TRANSLATE_NOOP("EditController", "Color range")
            )) {
            return;
        }
        const bool enabled = value >= 0.5;
        if (fine.color_range_enabled == enabled) {
            return;
        }
        const BackendGradeStack before = grade_stack_;
        fine.color_range_enabled = enabled;
        parameterEdited(parameter_key, before);
        return;
    } else {
        return;
    }

    if (target == nullptr || *target == value
        || !acceptParameter(value, minimum, maximum, label)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target = value;
    parameterEdited(parameter_key, before);
}

void EditController::setColorMixerValue(
    const int band_index,
    const QString& component,
    const double value
) {
    if (band_index < 0
        || static_cast<std::size_t>(band_index) >= BACKEND_COLOR_MIXER_BAND_COUNT
        || !acceptParameter(
            value,
            -1.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Color Mixer")
        )) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    const std::size_t index = static_cast<std::size_t>(band_index);
    double* target = nullptr;
    if (component == QStringLiteral("hue")) {
        target = &fine.mixer_hue[index];
    } else if (component == QStringLiteral("saturation")) {
        target = &fine.mixer_saturation[index];
    } else if (component == QStringLiteral("lightness")) {
        target = &fine.mixer_lightness[index];
    }
    if (target == nullptr || *target == value) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    *target = value;
    parameterEdited(
        QStringLiteral("color_mixer/%1/%2").arg(component).arg(band_index),
        before
    );
}

double EditController::selectiveColorValue(
    const int target_index,
    const int component_index
) const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || target_index < 0
        || static_cast<std::size_t>(target_index) >= BACKEND_SELECTIVE_COLOR_TARGET_COUNT
        || component_index < 0
        || static_cast<std::size_t>(component_index)
            >= BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT) {
        return 0.0;
    }
    const std::size_t index = static_cast<std::size_t>(target_index)
        * BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT
        + static_cast<std::size_t>(component_index);
    return grade_node->fine.selective_color_cmyk[index];
}

void EditController::setSelectiveColorValue(
    const int target_index,
    const int component_index,
    const double value
) {
    if (target_index < 0
        || static_cast<std::size_t>(target_index) >= BACKEND_SELECTIVE_COLOR_TARGET_COUNT
        || component_index < 0
        || static_cast<std::size_t>(component_index)
            >= BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT
        || !acceptParameter(
            value,
            -1.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Selective Color")
        )) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    const std::size_t index = static_cast<std::size_t>(target_index)
        * BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT
        + static_cast<std::size_t>(component_index);
    if (fine.selective_color_cmyk[index] == value) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    fine.selective_color_cmyk[index] = value;
    parameterEdited(
        QStringLiteral("selective_color/%1/%2").arg(target_index).arg(component_index),
        before
    );
}

bool EditController::selectiveColorRelative() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr || grade_node->fine.selective_color_relative;
}

void EditController::setSelectiveColorRelative(const bool relative) {
    auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || grade_node->fine.selective_color_relative == relative) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].fine.selective_color_relative = relative;
    parameterEdited(QStringLiteral("selective_color/method"), before);
}

void EditController::selectPointColor(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || index < 0 || index >= point_color_count(grade_node->fine)
        || index == selected_point_color_index_) {
        return;
    }
    finishActiveGesture();
    selected_point_color_index_ = index;
    notifyParametersChanged();
}

void EditController::removeSelectedPointColor() {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || selected_point_color_index_ < 0
        || selected_point_color_index_ >= point_color_count(grade_node->fine)) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    auto& fine = grade_node->fine;
    if (selected_point_color_index_ == 0) {
        if (!fine.additional_point_colors.isEmpty()) {
            const BackendPointColorRange promoted = fine.additional_point_colors.takeFirst();
            set_point_color_at(fine, 0, promoted);
        } else {
            fine.color_range_enabled = false;
            fine.color_range_center = 0.0;
            fine.color_range_width = 30.0;
            fine.color_range_softness = 0.5;
            fine.color_range_hue = 0.0;
            fine.color_range_saturation = 0.0;
            fine.color_range_lightness = 0.0;
        }
    } else {
        fine.additional_point_colors.removeAt(selected_point_color_index_ - 1);
    }
    const int count = point_color_count(fine);
    selected_point_color_index_ = count == 0
        ? -1 : std::min(selected_point_color_index_, count - 1);
    parameterEdited(QStringLiteral("point_color/remove"), before);
}

void EditController::setPointColorPickerActive(const bool active) {
    if (point_color_picker_active_ == active) {
        return;
    }
    point_color_picker_active_ = active;
    emit pointColorPickerActiveChanged();
    if (active && white_balance_picker_active_) {
        white_balance_picker_active_ = false;
        emit whiteBalancePickerActiveChanged();
    }
}

void EditController::setWhiteBalancePickerActive(const bool active) {
    if (white_balance_picker_active_ == active) {
        return;
    }
    white_balance_picker_active_ = active;
    emit whiteBalancePickerActiveChanged();
    if (active && point_color_picker_active_) {
        point_color_picker_active_ = false;
        emit pointColorPickerActiveChanged();
    }
}

void EditController::setWhiteBalanceFromPreview(
    const double normalized_x,
    const double normalized_y,
    const QString& preview_generation
) {
    if (!active_ || interactionLocked() || !hasSelectedGradeNode()
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y)
        || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    bool valid_generation = false;
    const quint64 visible_generation = preview_generation.toULongLong(&valid_generation);
    if (!valid_generation) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "White Balance needs a ready preview"
        )));
        return;
    }
    const auto snapshot = preview_store_->snapshot(
        EditPreviewSlot::Current, visible_generation
    );
    const QImage image = QImage::fromData(snapshot.bytes);
    if (image.isNull()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "White Balance needs a ready preview"
        )));
        return;
    }
    const int center_x = std::clamp(
        static_cast<int>(std::round(normalized_x * (image.width() - 1))),
        0, image.width() - 1
    );
    const int center_y = std::clamp(
        static_cast<int>(std::round(normalized_y * (image.height() - 1))),
        0, image.height() - 1
    );
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    int samples = 0;
    for (int y = std::max(0, center_y - 1);
         y <= std::min(image.height() - 1, center_y + 1); ++y) {
        for (int x = std::max(0, center_x - 1);
             x <= std::min(image.width() - 1, center_x + 1); ++x) {
            const QColor sample = image.pixelColor(x, y);
            red += sample.redF();
            green += sample.greenF();
            blue += sample.blueF();
            ++samples;
        }
    }
    const double inverse = 1.0 / static_cast<double>(samples);
    red = std::max(red * inverse, 1.0e-4);
    green = std::max(green * inverse, 1.0e-4);
    blue = std::max(blue * inverse, 1.0e-4);

    // The preview is already rendered with the current node. Convert the
    // sampled neutral error into an additive correction in the normalized
    // temperature/tint intent space. The image kernel remains authoritative
    // for the CAT16 transform applied by those parameters.
    const auto* const grade_node = selectedGradeNode();
    const double temperature_delta = -0.9 * std::log(red / blue);
    const double tint_delta = 0.9 * std::log(green / std::sqrt(red * blue));
    const BackendGradeStack before = grade_stack_;
    auto& basic = grade_stack_.grade_nodes[selected_grade_node_index_].basic;
    basic.white_balance_temperature = std::clamp(
        grade_node->basic.white_balance_temperature + temperature_delta,
        -1.0,
        1.0
    );
    basic.white_balance_tint = std::clamp(
        grade_node->basic.white_balance_tint + tint_delta,
        -1.0,
        1.0
    );
    setWhiteBalancePickerActive(false);
    parameterEdited(QStringLiteral("white_balance/picker"), before);
}

void EditController::addPointColorFromPreview(
    const double normalized_x,
    const double normalized_y,
    const QString& preview_generation
) {
    if (!active_ || interactionLocked() || !std::isfinite(normalized_x)
        || !std::isfinite(normalized_y) || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    bool valid_generation = false;
    const quint64 visible_generation = preview_generation.toULongLong(&valid_generation);
    if (!valid_generation) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color needs a ready preview"
        )));
        return;
    }
    const auto snapshot = preview_store_->snapshot(
        EditPreviewSlot::Current, visible_generation
    );
    const QImage image = QImage::fromData(snapshot.bytes);
    if (image.isNull()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color needs a ready preview"
        )));
        return;
    }
    const int center_x = std::clamp(
        static_cast<int>(std::round(normalized_x * (image.width() - 1))),
        0, image.width() - 1
    );
    const int center_y = std::clamp(
        static_cast<int>(std::round(normalized_y * (image.height() - 1))),
        0, image.height() - 1
    );
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    int samples = 0;
    for (int y = std::max(0, center_y - 1); y <= std::min(image.height() - 1, center_y + 1); ++y) {
        for (int x = std::max(0, center_x - 1); x <= std::min(image.width() - 1, center_x + 1); ++x) {
            const QColor sample = image.pixelColor(x, y);
            red += sample.redF();
            green += sample.greenF();
            blue += sample.blueF();
            ++samples;
        }
    }
    const double inverse = 1.0 / static_cast<double>(samples);
    const BackendPointColorRange range{
        .enabled = true,
        .center_degrees = sampled_oklch_hue(QColor::fromRgbF(
            static_cast<float>(red * inverse),
            static_cast<float>(green * inverse),
            static_cast<float>(blue * inverse)
        )),
    };
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    if (point_color_count(fine) >= MAX_POINT_COLOR_COUNT) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color supports at most 16 samples"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    if (point_color_count(fine) == 0) {
        set_point_color_at(fine, 0, range);
        selected_point_color_index_ = 0;
    } else {
        fine.additional_point_colors.push_back(range);
        selected_point_color_index_ = point_color_count(fine) - 1;
    }
    parameterEdited(QStringLiteral("point_color/add"), before);
    setPointColorPickerActive(false);
}

bool EditController::openPhoto(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_path,
    const QString& title,
    const QString& provisional_preview_source
) {
    if (photo_id.isEmpty() || representation_id.isEmpty() || source_path.isEmpty()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The selected Review item has no editable original source")));
        return false;
    }
    if (state_running_) {
        if (active_) {
            // Do not make a fast Library selection race a background state
            // operation. The newest target wins and is opened as soon as the
            // current operation reaches a safe controller boundary. Autosave
            // may need to chain once more if the user changed controls while
            // its snapshot was in flight.
            pending_photo_open_ = PendingPhotoOpen{
                .photo_id = photo_id,
                .representation_id = representation_id,
                .source_path = source_path,
                .title = title,
                .provisional_preview_source = provisional_preview_source,
            };
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Preparing the selected photo…"
            )));
            return true;
        }
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Finish the current version operation first")));
        return false;
    }
    if (active_ && photo_id == photo_id_
        && representation_id == representation_id_
        && source_path == source_path_) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "This photo is already open in Precision")));
        return true;
    }
    if (dirty_ && active_) {
        // Shadow's working ref is an autosave, not a manually committed version. Queue the
        // selected photo, force the pending working snapshot now, and resume this exact open
        // request once persistence succeeds. This is intentionally non-blocking for browsing.
        pending_photo_open_ = PendingPhotoOpen{
            .photo_id = photo_id,
            .representation_id = representation_id,
            .source_path = source_path,
            .title = title,
            .provisional_preview_source = provisional_preview_source,
        };
        autosave_debounce_.stop();
        if (autosaveFailed()) {
            // Do not silently retry a known permanent error on every library
            // selection. The shell can now offer retry, stay here, or an
            // explicit discard-and-open recovery action.
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Autosave failed · resolve it before replacing this photo's working changes"
            )));
            emit photoSwitchSaveFailed();
            // The selection has been accepted and is queued behind the
            // recovery choice surfaced by the shell. Returning success keeps
            // Main.qml from also reporting a generic "could not open" error.
            return true;
        }
        if (!autosave_requested_) {
            autosave_requested_ = true;
            emit autosavePendingChanged();
        }
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Saving current adjustments before opening the selected photo…"
        )));
        startAutosave();
        return true;
    }

    ++photo_generation_;
    ++render_revision_;
    active_parameter_gestures_.clear();
    working_revision_ = 0;
    autosave_snapshot_revision_ = 0;
    settled_render_revision_ = 0;
    preview_debounce_.stop();
    autosave_debounce_.stop();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
    resetDetailState();
    preview_queued_ = false;
    before_requested_ = false;
    clearHistograms();
    if (!optics_receipt_.isEmpty()) {
        optics_receipt_.clear();
        emit opticsReceiptChanged();
    }
    photo_id_ = photo_id;
    representation_id_ = representation_id;
    source_path_ = source_path;
    title_ = title;
    if (provisional_preview_source_ != provisional_preview_source) {
        provisional_preview_source_ = provisional_preview_source;
        emit provisionalPreviewSourceChanged();
    }
    versions_.replace({});
    base_commit_id_.clear();
    durable_working_commit_id_.clear();
    setVersionDraft(false);
    committed_grade_stack_ = {};
    clearSessionHistory();
    setGradeStack({});
    if (!preview_source_.isEmpty()) {
        preview_source_.clear();
        emit previewSourceChanged();
    }
    if (!before_preview_source_.isEmpty()) {
        before_preview_source_.clear();
        emit beforePreviewSourceChanged();
    }
    if (!before_error_message_.isEmpty()) {
    before_error_message_.clear();
        emit beforeErrorTextChanged();
    }
    if (!recipe_recovery_message_.isEmpty()) {
        recipe_recovery_message_.clear();
        emit recipeRecoveryChanged();
    }
    preview_store_->clearAll(render_revision_, photo_generation_);
    if (!active_) {
        active_ = true;
        emit activeChanged();
        emit gradeNodeActionsChanged();
    }
    emit titleChanged();
    emit sourcePathChanged();
    emit sourceIdentityChanged();
    state_task_kind_ = EditStateTaskKind::Open;
    setStateRunning(true);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Loading non-destructive edit history…")));
    state_watcher_.setFuture(QtConcurrent::run(
        load_state,
        backend_,
        photo_id_,
        source_path_,
        photo_generation_
    ));
    return true;
}

void EditController::closePhoto() {
    if (state_running_) {
        // A return to Library is allowed while an initial open or an autosave
        // is in flight. A later photo selection can install a fresh pending
        // target; otherwise the completed task will close this session.
        pending_photo_open_.reset();
        close_photo_after_autosave_ = true;
        return;
    }
    // Recovery belongs to the Precision session, not to the photo in the
    // Library. Clear it before the inactive early-return as an open failure
    // deliberately marks the editor inactive while leaving recovery visible.
    // Otherwise "Return to Review" changes the workspace behind a modal popup
    // which can no longer be dismissed.
    if (!recipe_recovery_message_.isEmpty()) {
        recipe_recovery_message_.clear();
        emit recipeRecoveryChanged();
    }
    if (dirty_) {
        if (!autosave_requested_) {
            // Merely previewing an older named Version is a transient draft,
            // not an edit. Closing it must not silently replace `working`.
            revertEdits();
        } else {
            close_photo_after_autosave_ = true;
            autosave_debounce_.stop();
            startAutosave();
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Saving current adjustments before closing Precision…")));
            return;
        }
    }
    if (!active_) {
        return;
    }
    pending_photo_open_.reset();
    setPointColorPickerActive(false);
    setWhiteBalancePickerActive(false);
    clearSessionHistory();
    resetDetailState();
    clearHistograms();
    if (!optics_receipt_.isEmpty()) {
        optics_receipt_.clear();
        emit opticsReceiptChanged();
    }
    if (!provisional_preview_source_.isEmpty()) {
        provisional_preview_source_.clear();
        emit provisionalPreviewSourceChanged();
    }
    // Precision is a session, not a hidden second Library. Drop its published
    // sources on exit so reopening from a different grid item can never show
    // the previous image while the next state request is loading.
    if (!preview_source_.isEmpty()) {
        preview_source_.clear();
        emit previewSourceChanged();
    }
    if (!before_preview_source_.isEmpty()) {
        before_preview_source_.clear();
        emit beforePreviewSourceChanged();
    }
    if (!before_error_message_.isEmpty()) {
        before_error_message_.clear();
        emit beforeErrorTextChanged();
    }
    if (!photo_id_.isEmpty() || !representation_id_.isEmpty()) {
        photo_id_.clear();
        representation_id_.clear();
        emit sourceIdentityChanged();
    }
    if (!source_path_.isEmpty()) {
        source_path_.clear();
        emit sourcePathChanged();
    }
    if (!title_.isEmpty()) {
        title_.clear();
        emit titleChanged();
    }
    active_ = false;
    emit activeChanged();
    emit gradeNodeActionsChanged();
    emit historyChanged();
}

void EditController::resetIncompatibleRecipe() {
    if (!recipeRecoveryRequired() || state_running_ || photo_id_.isEmpty()
        || source_path_.isEmpty()) {
        return;
    }
    state_task_kind_ = EditStateTaskKind::ResetIncompatibleRecipe;
    setStateRunning(true);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Resetting this photo’s development edits…"
    )));
    state_watcher_.setFuture(QtConcurrent::run(
        reset_incompatible_recipe_state,
        backend_,
        photo_id_,
        source_path_,
        photo_generation_
    ));
}

void EditController::selectGradeNode(const int index) {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    if (!active_ || interactionLocked() || index < 0 || index >= count
        || index == selected_grade_node_index_) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setWhiteBalancePickerActive(false);
    setGradeStack(grade_stack_, grade_stack_.grade_nodes.at(index).grade_node_id);
}

void EditController::addGradeNode() {
    if (!canAddGradeNode()) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "An edit can contain at most 16 Grade Nodes")));
        return;
    }
    finishActiveGesture();
    BackendGradeNode grade_node;
    try {
        grade_node = backend_->newBasicGradeNode(
            uniqueGradeNodeLabel(QStringLiteral("Adjustments"))
        );
    } catch (const std::exception& error) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController",
                          "Could not create Grade Node · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, grade_node, selection)) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "The Grade Node could not be inserted safely")));
        return;
    }
    setGradeStack(std::move(updated), grade_node.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/add").arg(grade_node.grade_node_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Added Grade Node · %1"),
      {display_grade_node_label(grade_node.label)}));
}

void EditController::duplicateSelectedGradeNode() {
    const auto* const source = selectedGradeNode();
    if (!canAddGradeNode() || source == nullptr) {
        return;
    }
    finishActiveGesture();
    BackendGradeNode duplicate;
    try {
        duplicate = backend_->newBasicGradeNode(
            uniqueGradeNodeLabel(source->label + QStringLiteral(" Copy"))
        );
    } catch (const std::exception& error) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController",
                          "Could not duplicate Grade Node · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    duplicate.basic = source->basic;
    duplicate.fine = source->fine;
    duplicate.enabled = source->enabled;

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, duplicate, selection)) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "The duplicate Grade Node could not be inserted safely")));
        return;
    }
    setGradeStack(std::move(updated), duplicate.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/duplicate").arg(duplicate.grade_node_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Duplicated Grade Node · %1"),
      {display_grade_node_label(duplicate.label)}));
}

void EditController::deleteSelectedGradeNode() {
    const auto* const selected = selectedGradeNode();
    if (!canDeleteGradeNode() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString deleted_id = selected->grade_node_id;
    const QString deleted_label = selected->label;
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::deleteSelection(updated, selection)) {
        return;
    }
    const QString next_id = updated.grade_nodes.at(selection).grade_node_id;
    setGradeStack(std::move(updated), next_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/delete").arg(deleted_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Deleted Grade Node · %1"),
      {display_grade_node_label(deleted_label)}));
}

void EditController::moveSelectedGradeNode(const int destination_index) {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString moved_id = selected->grade_node_id;
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::moveSelection(updated, selection, destination_index)) {
        return;
    }
    setGradeStack(std::move(updated), moved_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/move").arg(moved_id),
        before
    );
    schedulePreview(0);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Reordered Grade Node")));
}

void EditController::beginParameterEdit(const QString& parameter_key) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    active_parameter_gestures_.insert(parameter_key);
    history_.beginGesture(gradeNodeHistoryKey(parameter_key).toStdString(), grade_stack_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::endParameterEdit(const QString& parameter_key) {
    if (!active_ || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.endGesture(gradeNodeHistoryKey(parameter_key).toStdString(), grade_stack_);
    const bool ended_active_gesture = active_parameter_gestures_.remove(parameter_key) > 0;
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
    if (ended_active_gesture && active_parameter_gestures_.isEmpty()) {
        // Replace the low-latency gesture proxy with a normal-resolution
        // frame for the exact final slider value.
        schedulePreview(0);
    }
}

void EditController::beginToneCurveGesture(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || !tone_curve_points_.isEditable()
        || !tone_curve_points_.selectPoint(index)) {
        return;
    }
    beginParameterEdit(tone_curve_gesture_key(index));
}

void EditController::moveToneCurvePoint(
    const int index,
    const double x,
    const double y
) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (!tone_curve_points_.movePoint(index, x, y)) {
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    ensure_oklab_lightness_curve(edited.fine);
    edited.fine.oklab_lightness_curve_points =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    toneCurveEdited(
        tone_curve_gesture_key(index),
        before,
        EDIT_PREVIEW_THROTTLE_MS
    );
}

void EditController::endToneCurveGesture(const int index) {
    endParameterEdit(tone_curve_gesture_key(index));
}

void EditController::addToneCurvePoint(const double x, const double y) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (tone_curve_points_.addPoint(x, y) < 0) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "The point cannot be added inside this curve")));
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    ensure_oklab_lightness_curve(edited.fine);
    edited.fine.oklab_lightness_curve_points =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    toneCurveEdited(
        QStringLiteral("perceptual_tone_curve/add"),
        before,
        0
    );
}

void EditController::removeToneCurvePoint(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (!tone_curve_points_.removePoint(index)) {
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    ensure_oklab_lightness_curve(edited.fine);
    edited.fine.oklab_lightness_curve_points =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    clear_neutral_oklab_lightness_curve(edited.fine);
    toneCurveEdited(
        QStringLiteral("perceptual_tone_curve/remove"),
        before,
        0
    );
}

void EditController::resetToneCurve() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || !hasToneCurve()) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    tone_curve_points_.resetLinear();
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    edited.fine.oklab_lightness_curve_points.clear();
    toneCurveEdited(
        QStringLiteral("perceptual_tone_curve/reset"),
        before,
        0
    );
}

void EditController::undo() {
    if (!active_ || interactionLocked()) {
        return;
    }
    std::string history_key;
    const auto restored = history_.undo(grade_stack_, &history_key);
    emit historyChanged();
    if (!restored) {
        return;
    }
    QString preferred_id = history_grade_node_id(history_key);
    const bool undoes_insert = history_key.ends_with("/add")
        || history_key.ends_with("/duplicate");
    if (undoes_insert
        && GradeNodeStack::gradeNodeIndex(*restored, preferred_id) < 0
        && !restored->grade_nodes.isEmpty()) {
        const int previous_index = std::clamp(
            selected_grade_node_index_ - 1,
            0,
            static_cast<int>(restored->grade_nodes.size() - 1)
        );
        preferred_id = restored->grade_nodes.at(previous_index).grade_node_id;
    }
    autosave_requested_ = true;
    clearAutosaveFailure();
    ++working_revision_;
    setGradeStack(*restored, preferred_id);
    schedulePreview(0);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Undid the last session adjustment")));
}

void EditController::redo() {
    if (!active_ || interactionLocked()) {
        return;
    }
    std::string history_key;
    const auto restored = history_.redo(grade_stack_, &history_key);
    emit historyChanged();
    if (!restored) {
        return;
    }
    autosave_requested_ = true;
    clearAutosaveFailure();
    ++working_revision_;
    setGradeStack(*restored, history_grade_node_id(history_key));
    schedulePreview(0);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Redid the last session adjustment")));
}

void EditController::resetSelectedGradeNode() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset = grade_stack_;
    if (!GradeNodeStack::resetSelection(reset, selected_grade_node_index_)) {
        return;
    }
    const QString grade_node_id = grade_node->grade_node_id;
    setGradeStack(std::move(reset), grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/reset").arg(grade_node_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Reset the selected Grade Node")));
}

void EditController::revertEdits() {
    if (!active_ || state_running_) {
        return;
    }
    if (version_draft_) {
        setVersionDraft(false);
        base_commit_id_ = durable_working_commit_id_;
        versions_.setSelectedCommit(durable_working_commit_id_);
        setGradeStack(committed_grade_stack_);
        clearSessionHistory();
        schedulePreview(0);
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Discarded working changes and restored the Library version")));
        return;
    }
    if (grade_stack_ != committed_grade_stack_) {
        const BackendGradeStack before = grade_stack_;
        setGradeStack(committed_grade_stack_);
        recordWorkingTransition(QStringLiteral("revert"), before);
        schedulePreview(0);
    }
    autosave_debounce_.stop();
    clearAutosaveFailure();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Restored the current saved version")));
}

void EditController::requestBeforePreview() {
    if (!active_ || !before_preview_source_.isEmpty()) {
        return;
    }
    if (!before_error_message_.isEmpty()) {
    before_error_message_.clear();
        emit beforeErrorTextChanged();
    }
    before_requested_ = true;
    markHistogramUpdating(EditPreviewKind::NeutralBefore);
    maybeStartBeforePreview();
}

void EditController::requestDetailViewport(
    const double center_x,
    const double center_y,
    const int viewport_width_pixels,
    const int viewport_height_pixels
) {
    if (!active_ || !std::isfinite(center_x) || !std::isfinite(center_y)
        || center_x < 0.0 || center_x > 1.0 || center_y < 0.0 || center_y > 1.0
        || viewport_width_pixels <= 0 || viewport_height_pixels <= 0
        || viewport_width_pixels > 8'192 || viewport_height_pixels > 8'192) {
        return;
    }
    detail_center_x_ = center_x;
    detail_center_y_ = center_y;
    detail_viewport_width_ = static_cast<std::uint32_t>(viewport_width_pixels);
    detail_viewport_height_ = static_cast<std::uint32_t>(viewport_height_pixels);
    ++detail_viewport_revision_;
    if (!detail_mode_) {
        detail_mode_ = true;
        emit detailModeChanged();
    }
    if (!detail_error_message_.isEmpty()) {
    detail_error_message_.clear();
        emit detailErrorTextChanged();
    }
    invalidateDetailPresentation(false);
    detail_queued_ = true;
    detail_debounce_.start(EDIT_DETAIL_DEBOUNCE_MS);
}

void EditController::leaveDetailMode() {
    if (!detail_mode_ && detail_tiles_.isEmpty()) {
        return;
    }
    detail_debounce_.stop();
    detail_queued_ = false;
    ++detail_viewport_revision_;
    invalidateDetailPresentation();
    if (detail_mode_) {
        detail_mode_ = false;
        emit detailModeChanged();
    }
    if (!detail_error_message_.isEmpty()) {
    detail_error_message_.clear();
        emit detailErrorTextChanged();
    }
}

void EditController::saveVersion(const QString& version_name) {
    const QString name = version_name.trimmed();
    if (!active_ || state_running_) {
        return;
    }
    if (name.isEmpty()) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController", "Enter a name for this version")));
        return;
    }
    history_.finishGesture(grade_stack_);
    emit historyChanged();
    state_task_kind_ = EditStateTaskKind::Save;
    setStateRunning(true);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Creating Library version “%1”…"),
      {name}));
    state_watcher_.setFuture(QtConcurrent::run(
        save_state,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        durable_working_commit_id_,
        grade_stack_,
        name,
        photo_generation_
    ));
}

void EditController::loadVersionDraft(const QString& commit_id) {
    if (!active_ || state_running_ || commit_id.isEmpty()) {
        return;
    }
    if (dirty_) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Saving current adjustments before loading another version")));
        autosave_debounce_.stop();
        startAutosave();
        return;
    }
    state_task_kind_ = EditStateTaskKind::LoadDraft;
    setStateRunning(true);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Loading saved version into working changes…")));
    state_watcher_.setFuture(QtConcurrent::run(
        load_version_draft_state,
        backend_,
        photo_id_,
        source_path_,
        commit_id,
        photo_generation_
    ));
}

void EditController::retryAutosave() {
    if (!active_ || !dirty_ || !autosave_requested_ || state_running_) {
        return;
    }
    startAutosave();
}

void EditController::cancelPendingPhotoOpen() {
    pending_photo_open_.reset();
}

bool EditController::discardFailedAutosaveAndOpenPendingPhoto() {
    if (!autosaveFailed() || state_running_ || !pending_photo_open_.has_value()) {
        return false;
    }
    // This is reached only from the explicit destructive recovery action in
    // Main.qml. The durable `working` snapshot remains untouched; only the
    // unpersisted in-memory draft is discarded.
    autosave_debounce_.stop();
    clearAutosaveFailure();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
    setDirty(false);
    return openPendingPhoto();
}

bool EditController::prepareToClose() {
    autosave_debounce_.stop();
    preview_debounce_.stop();
    detail_debounce_.stop();
    preview_queued_ = false;
    before_requested_ = false;
    detail_queued_ = false;
    // A close must not race a queued photo selection. The existing session
    // still gets its durable working snapshot, but no new Precision session is
    // started on the way out.
    pending_photo_open_.reset();
    close_after_autosave_ = true;
    if (state_running_) {
        return false;
    }
    if (active_ && dirty_ && autosave_requested_) {
        // An autosave failure is sticky until the user explicitly retries it.
        // Retrying it implicitly from every native close event used to trap the
        // window in an endless "save failed -> try to quit -> save failed"
        // loop. Keep the working draft intact and let the shell offer the
        // deliberate choices: retry, keep editing, or quit without the last
        // unsaved working snapshot.
        if (autosaveFailed()) {
            close_after_autosave_ = false;
            emit closeSaveFailed();
            return false;
        }
        startAutosave();
        return false;
    }
    if (current_rendering_ || before_rendering_ || detail_rendering_) {
        return false;
    }
    close_after_autosave_ = false;
    return true;
}

void EditController::finishStateTask() {
    EditStateTaskResult result = state_watcher_.result();
    setStateRunning(false);
    if (result.photo_generation != photo_generation_) {
        maybeFinishDeferredApplicationClose();
        return;
    }
    if (!result.error.isEmpty()) {
        if (result.kind == EditStateTaskKind::Open && active_) {
            active_ = false;
            emit activeChanged();
            emit gradeNodeActionsChanged();
        }
        const bool newer_draft_exists = result.kind == EditStateTaskKind::Autosave
            && active_ && dirty_ && autosave_requested_
            && working_revision_ != autosave_snapshot_revision_;
        if (newer_draft_exists) {
            // This task was saving an older slider snapshot. It may legitimately lose a
            // compare-and-swap race while the user has already made a newer edit, so give that
            // newer snapshot one clean attempt before reporting a durable save failure.
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Saving newer adjustments locally…"
            )));
            if (pending_photo_open_.has_value() || close_photo_after_autosave_
                || close_after_autosave_) {
                startAutosave();
            } else {
                scheduleAutosave();
            }
            return;
        }
        if (result.kind == EditStateTaskKind::Autosave) {
            const LocalizedUiMessage failure = edit_message(
                QT_TRANSLATE_NOOP("EditController", "Autosave failed · %1"),
                {result.error}
            );
            setAutosaveFailure(failure);
            setStatusMessage(failure);
        } else if (result.kind == EditStateTaskKind::Open
                   && incompatible_development_recipe(result.error)) {
            recipe_recovery_message_ = edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "This photo uses an earlier development edit recipe that this build cannot read. Resetting removes only this photo’s edit history; the original file, Library metadata, ratings, flags, and albums are unchanged."
            ));
            emit recipeRecoveryChanged();
            setStatusMessage(recipe_recovery_message_);
        } else if (result.kind == EditStateTaskKind::ResetIncompatibleRecipe) {
            recipe_recovery_message_ = edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Could not reset this photo’s old development edits · %1"
            ), {result.error});
            emit recipeRecoveryChanged();
            setStatusMessage(recipe_recovery_message_);
        } else if (result.kind == EditStateTaskKind::Open) {
            setStatusMessage(edit_message(
                QT_TRANSLATE_NOOP("EditController", "Could not open this photo · %1"),
                {result.error}
            ));
        } else {
            setStatusMessage(edit_message(
                QT_TRANSLATE_NOOP("EditController", "Version operation failed · %1"),
                {result.error}
            ));
        }
        if (close_after_autosave_) {
            close_after_autosave_ = false;
            emit closeSaveFailed();
        }
        if (result.kind == EditStateTaskKind::Autosave && pending_photo_open_.has_value()) {
            emit photoSwitchSaveFailed();
        }
        close_photo_after_autosave_ = false;
        if (result.kind == EditStateTaskKind::Autosave) {
            // A persistent Catalog or decoder error must not look like an endless save.
            // Keep the draft intact and retry only after the user explicitly asks, or edits
            // again and therefore supplies a newer working snapshot.
            autosave_debounce_.stop();
            emit autosavePendingChanged();
        }
        if (result.kind == EditStateTaskKind::Open && openPendingPhoto()) {
            return;
        }
        if (preview_queued_) {
            preview_debounce_.start(0);
        }
        maybeStartBeforePreview();
        maybeStartDetailRender();
        return;
    }
    bool autosave_needs_follow_up = false;
    if (result.kind == EditStateTaskKind::Autosave) {
        autosave_needs_follow_up = applyAutosavedState(std::move(result.state));
        if (autosave_needs_follow_up) {
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Saving newer adjustments locally…")));
            if (pending_photo_open_.has_value() || close_photo_after_autosave_
                || close_after_autosave_) {
                startAutosave();
            } else {
                scheduleAutosave();
            }
            return;
        }
    } else {
        applyState(std::move(result.state));
    }
    if (result.kind == EditStateTaskKind::ResetIncompatibleRecipe) {
        if (!recipe_recovery_message_.isEmpty()) {
            recipe_recovery_message_.clear();
            emit recipeRecoveryChanged();
        }
        if (!active_) {
            active_ = true;
            emit activeChanged();
            emit gradeNodeActionsChanged();
        }
    }
    if (openPendingPhoto()) {
        return;
    }
    switch (result.kind) {
    case EditStateTaskKind::Open:
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Edit history ready · rendering preview")));
        if (!close_after_autosave_) {
            schedulePreview(0);
        }
        break;
    case EditStateTaskKind::ResetIncompatibleRecipe:
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Old development edits reset · rendering the current recipe")));
        if (!close_after_autosave_) {
            schedulePreview(0);
        }
        break;
    case EditStateTaskKind::Save:
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Library version created · the previous state remains available")));
        break;
    case EditStateTaskKind::Autosave:
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Current adjustments saved locally")));
        break;
    case EditStateTaskKind::LoadDraft:
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Named version loaded as a draft · adjust it to create a new working state")));
        if (!close_after_autosave_) {
            schedulePreview(0);
        }
        break;
    }
    if (!close_after_autosave_ && preview_queued_ && !preview_debounce_.isActive()) {
        preview_debounce_.start(0);
    }
    if (!close_after_autosave_) {
        maybeStartBeforePreview();
        maybeStartDetailRender();
    }
    if (close_photo_after_autosave_) {
        close_photo_after_autosave_ = false;
        closePhoto();
    }
    maybeFinishDeferredApplicationClose();
}

bool EditController::openPendingPhoto() {
    if (close_after_autosave_ || !pending_photo_open_.has_value()) {
        return false;
    }
    const PendingPhotoOpen pending = std::move(*pending_photo_open_);
    pending_photo_open_.reset();
    close_photo_after_autosave_ = false;
    return openPhoto(
        pending.photo_id,
        pending.representation_id,
        pending.source_path,
        pending.title,
        pending.provisional_preview_source
    );
}

void EditController::maybeFinishDeferredApplicationClose() {
    if (!close_after_autosave_ || state_running_ || current_rendering_
        || before_rendering_ || detail_rendering_) {
        return;
    }
    close_after_autosave_ = false;
    emit closeReady();
}

void EditController::finishPreviewTask() {
    EditPreviewTaskResult result = preview_watcher_.result();
    setPreviewRunning(result.generation.kind, false);
    if (close_after_autosave_) {
        preview_queued_ = false;
        before_requested_ = false;
        detail_queued_ = false;
        maybeFinishDeferredApplicationClose();
        return;
    }
    const bool accepted = active_ && accepts_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_
    );
    const bool presentable_current = active_ && can_present_edit_preview(
        result.generation,
        photo_generation_,
        render_revision_
    );

    if (result.generation.kind == EditPreviewKind::Current && presentable_current) {
        if (accepted) {
            settled_render_revision_ = result.generation.current_revision;
        }
        if (!result.error.isEmpty()) {
            if (accepted) {
                markHistogramFailed(EditPreviewKind::Current);
                if (raw_development_unavailable(result.error)) {
                    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                        "EditController",
                        "This RAW can be browsed from its embedded preview, but the active local decoder cannot develop it for Precision. Use a compatible local RAW provider or convert it to DNG."
                    )));
                } else {
                    setStatusMessage(edit_message(
                        QT_TRANSLATE_NOOP(
                            "EditController",
                            "Preview render failed · %1"
                        ),
                        {result.error}
                    ));
                }
            }
        } else {
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            preview_store_->publish(
                EditPreviewSlot::Current,
                std::move(result.preview.bytes),
                dimensions,
                std::move(result.preview.display_zebra),
                std::move(result.preview.luma_waveform),
                result.generation.current_revision
            );
            preview_source_ = QStringLiteral("image://shadow-edit/current?generation=%1")
                                  .arg(result.generation.current_revision);
            emit previewSourceChanged();
            if (!provisional_preview_source_.isEmpty()) {
                // Publish the authoritative RAW render first, so QML never reveals an empty
                // canvas between the cached Library visual and the local edit preview.
                provisional_preview_source_.clear();
                emit provisionalPreviewSourceChanged();
            }
            if (accepted) {
                const QVariantMap new_receipt = optics_receipt_map(result.preview.optics);
                if (optics_receipt_ != new_receipt) {
                    optics_receipt_ = new_receipt;
                    emit opticsReceiptChanged();
                }
                publishHistogram(
                    EditPreviewKind::Current,
                    result.preview.analysis,
                    result.generation.current_revision
                );
                if (!autosaveFailed()) {
                    setStatusMessage(edit_message(
                        dirty_ ? QT_TRANSLATE_NOOP(
                                     "EditController",
                                     "Saving adjustments · preview is current"
                                 )
                               : QT_TRANSLATE_NOOP(
                                     "EditController",
                                     "Working state and preview are current"
                                 )
                    ));
                }
            }
        }
    } else if (result.generation.kind == EditPreviewKind::NeutralBefore && accepted) {
        before_requested_ = false;
        if (!result.error.isEmpty()) {
            markHistogramFailed(EditPreviewKind::NeutralBefore);
      before_error_message_ = edit_message(
          QT_TRANSLATE_NOOP("EditController", "Neutral baseline failed · %1"),
          {result.error});
            emit beforeErrorTextChanged();
        } else {
            const QSize dimensions(
                static_cast<int>(result.preview.width),
                static_cast<int>(result.preview.height)
            );
            preview_store_->publish(
                EditPreviewSlot::Before,
                std::move(result.preview.bytes),
                dimensions,
                std::move(result.preview.display_zebra),
                std::move(result.preview.luma_waveform),
                result.generation.photo
            );
            publishHistogram(
                EditPreviewKind::NeutralBefore,
                result.preview.analysis,
                result.generation.photo
            );
            before_preview_source_ = QStringLiteral(
                "image://shadow-edit/before?generation=%1"
            ).arg(result.generation.photo);
            emit beforePreviewSourceChanged();
        }
    }

    if (preview_queued_) {
        preview_queued_ = false;
        // The edit that queued this render already advanced render_revision_.
        // Start the latest snapshot without inventing another generation.
        preview_debounce_.start(0);
    } else {
        maybeStartBeforePreview();
        maybeStartDetailRender();
        if (accepted && result.generation.kind == EditPreviewKind::Current
            && result.error.isEmpty()) {
            scheduleDetailWarmup();
        }
    }
    maybeFinishDeferredApplicationClose();
}

void EditController::finishDetailTask() {
    EditDetailTaskResult result = detail_watcher_.result();
    setDetailRunning(false);
    if (close_after_autosave_) {
        detail_queued_ = false;
        before_requested_ = false;
        maybeFinishDeferredApplicationClose();
        return;
    }
    const bool accepted = detail_mode_ && active_ && accepts_edit_detail(
        result.generation,
        photo_generation_,
        render_revision_,
        detail_viewport_revision_
    );
    if (accepted) {
        if (!result.error.isEmpty()) {
      detail_error_message_ = edit_message(
          QT_TRANSLATE_NOOP("EditController", "Full detail failed · %1"),
          {result.error});
            emit detailErrorTextChanged();
        } else {
            const auto* tile = result.viewport.tiles.size() == 1
                ? &result.viewport.tiles.front()
                : nullptr;
            const std::uint64_t expected_stride = tile == nullptr
                ? 0U
                : static_cast<std::uint64_t>(tile->width) * 3U;
            const std::uint64_t expected_bytes = tile == nullptr
                ? 0U
                : expected_stride * tile->height;
            const bool valid = result.viewport.full_width > 0
                && result.viewport.full_height > 0 && tile != nullptr
                && tile->width > 0 && tile->height > 0
                && tile->row_stride_bytes == expected_stride
                && expected_bytes == static_cast<std::uint64_t>(tile->bytes.size());
            QVector<EditPreviewStore::DetailPublication> publications;
            QVariantList presentation;
            if (valid) {
                auto& mutable_tile = result.viewport.tiles.front();
                const QString ticket = QStringLiteral("viewport-%1-%2")
                                           .arg(mutable_tile.x)
                                           .arg(mutable_tile.y);
                publications.push_back({
                    .ticket = ticket,
                    .bytes = std::move(mutable_tile.bytes),
                    .dimensions = QSize(
                        static_cast<int>(mutable_tile.width),
                        static_cast<int>(mutable_tile.height)
                    ),
                    .row_stride_bytes = static_cast<qsizetype>(
                        mutable_tile.row_stride_bytes
                    ),
                });
                QVariantMap item;
                item.insert(QStringLiteral("x"), mutable_tile.x);
                item.insert(QStringLiteral("y"), mutable_tile.y);
                item.insert(QStringLiteral("width"), mutable_tile.width);
                item.insert(QStringLiteral("height"), mutable_tile.height);
                item.insert(
                    QStringLiteral("source"),
                    QStringLiteral(
                        "image://shadow-edit/detail/%1?photo=%2&recipe=%3&viewport=%4"
                    )
                        .arg(ticket)
                        .arg(result.generation.photo)
                        .arg(result.generation.recipe_revision)
                        .arg(result.generation.viewport_revision)
                );
                presentation.push_back(item);
            }
            if (!valid) {
        detail_error_message_ = edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Full detail returned an invalid RGB8 tile layout"));
                emit detailErrorTextChanged();
            } else {
                const bool geometry_changed = detail_full_width_
                        != result.viewport.full_width
                    || detail_full_height_ != result.viewport.full_height
                    || detail_retained_bytes_ != result.viewport.retained_bytes;
                detail_full_width_ = result.viewport.full_width;
                detail_full_height_ = result.viewport.full_height;
                detail_retained_bytes_ = result.viewport.retained_bytes;
                preview_store_->publishDetails(
                    std::move(publications),
                    result.generation
                );
                detail_tiles_ = std::move(presentation);
                if (geometry_changed) {
                    emit detailGeometryChanged();
                }
                emit detailTilesChanged();
                const double retained_mib = static_cast<double>(detail_retained_bytes_)
                    / (1'024.0 * 1'024.0);
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP(
                "EditController",
                "Full-resolution detail ready · %1 MiB local source"),
            {LocalizedUiArgument::formattedNumber(retained_mib, 'f', 0)})
                );
            }
        }
    }

    if (detail_queued_) {
        maybeStartDetailRender();
    } else {
        maybeStartBeforePreview();
    }
    maybeFinishDeferredApplicationClose();
}

void EditController::startPreviewRender() {
    if (!active_ || state_running_) {
        preview_queued_ = active_;
        return;
    }
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
        return;
    }
    setPreviewRunning(EditPreviewKind::Current, true);
    preview_queued_ = false;
    const bool interactive = !active_parameter_gestures_.isEmpty();
    const std::uint32_t max_edge = interactive
        ? EDIT_INTERACTIVE_PREVIEW_EDGE : EDIT_PREVIEW_EDGE;
    const std::uint8_t jpeg_quality = interactive
        ? EDIT_INTERACTIVE_PREVIEW_QUALITY : EDIT_PREVIEW_QUALITY;
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Rendering preview…")));
    preview_watcher_.setFuture(QtConcurrent::run(
        render_preview,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        grade_stack_,
        max_edge,
        jpeg_quality,
        EditPreviewGeneration{
            .kind = EditPreviewKind::Current,
            .photo = photo_generation_,
            .current_revision = render_revision_,
        }
    ));
}

void EditController::startDetailRender() {
    if (!detail_mode_ || !active_) {
        detail_queued_ = false;
        return;
    }
    if (state_running_ || current_rendering_ || before_rendering_
        || settled_render_revision_ != render_revision_ || detail_rendering_) {
        detail_queued_ = true;
        return;
    }
    detail_queued_ = false;
    setDetailRunning(true);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Preparing exact full-resolution detail…")));
    detail_watcher_.setFuture(QtConcurrent::run(
        render_detail,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        grade_stack_,
        detail_render_token_,
        detail_center_x_,
        detail_center_y_,
        detail_viewport_width_,
        detail_viewport_height_,
        EditDetailGeneration{
            .photo = photo_generation_,
            .recipe_revision = render_revision_,
            .viewport_revision = detail_viewport_revision_,
        }
    ));
}

void EditController::scheduleDetailWarmup() {
    if (!active_ || detail_mode_ || state_running_ || current_rendering_
        || detail_rendering_ || settled_render_revision_ != render_revision_
        || detail_warmup_watcher_.isRunning()) {
        return;
    }
    detail_warmup_debounce_.start(EDIT_DETAIL_WARMUP_IDLE_MS);
}

void EditController::startDetailWarmup() {
    if (!active_ || detail_mode_ || state_running_ || current_rendering_
        || before_rendering_ || detail_rendering_
        || settled_render_revision_ != render_revision_
        || detail_warmup_watcher_.isRunning()) {
        return;
    }
    // This uses the same global cancellation source as foreground detail.
    // Any later pan, zoom, Recipe edit, or photo switch increments it and
    // causes this idle request to be discarded between tiles.
    detail_warmup_token_ = backend_->beginEditDetailRequest();
    detail_warmup_watcher_.setFuture(QtConcurrent::run(
        warm_detail_source,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        grade_stack_,
        detail_warmup_token_,
        photo_generation_,
        render_revision_
    ));
}

void EditController::finishDetailWarmupTask() {
    const EditDetailWarmupTaskResult result = detail_warmup_watcher_.result();
    if (result.photo_generation != photo_generation_
        || result.render_revision != render_revision_
        || result.error.startsWith(QStringLiteral("full detail render was superseded"))) {
        return;
    }
    // Deliberately no status transition. A successful warmup is an invisible
    // cache hit for the next 100% request; a provider failure remains visible
    // only if the user explicitly asks to enter full-detail mode.
}

void EditController::maybeStartBeforePreview() {
    if (detail_rendering_) {
        return;
    }
    if (!can_start_neutral_before(NeutralBeforeStartState{
            .requested = before_requested_,
            .active = active_,
            .state_task_running = state_running_,
            .current_rendering = current_rendering_,
            .before_rendering = before_rendering_,
            .current_scheduled = preview_debounce_.isActive() || preview_queued_,
            .settled_current_revision = settled_render_revision_,
            .current_revision = render_revision_,
        })) {
        return;
    }
    setPreviewRunning(EditPreviewKind::NeutralBefore, true);
    preview_watcher_.setFuture(QtConcurrent::run(
        render_preview,
        backend_,
        photo_id_,
        source_path_,
        QString{},
        BackendGradeStack{},
        EDIT_PREVIEW_EDGE,
        EDIT_PREVIEW_QUALITY,
        EditPreviewGeneration{
            .kind = EditPreviewKind::NeutralBefore,
            .photo = photo_generation_,
            .current_revision = 0,
        }
    ));
}

void EditController::maybeStartDetailRender() {
    if (!detail_queued_ || !detail_mode_ || detail_rendering_
        || detail_debounce_.isActive()) {
        return;
    }
    if (state_running_ || current_rendering_ || before_rendering_
        || settled_render_revision_ != render_revision_) {
        return;
    }
    detail_debounce_.start(0);
}

void EditController::invalidateDetailPresentation(const bool discard_tiles) {
    detail_render_token_ = backend_->beginEditDetailRequest();
    if (discard_tiles) {
        preview_store_->clearDetails(EditDetailGeneration{
            .photo = photo_generation_,
            .recipe_revision = render_revision_,
            .viewport_revision = detail_viewport_revision_,
        });
        if (!detail_tiles_.isEmpty()) {
            detail_tiles_.clear();
            emit detailTilesChanged();
        }
    }
}

void EditController::resetDetailState() {
    detail_debounce_.stop();
    detail_warmup_debounce_.stop();
    detail_queued_ = false;
    ++detail_viewport_revision_;
    invalidateDetailPresentation();
    if (detail_mode_) {
        detail_mode_ = false;
        emit detailModeChanged();
    }
    if (!detail_error_message_.isEmpty()) {
    detail_error_message_.clear();
        emit detailErrorTextChanged();
    }
    const bool had_geometry = detail_full_width_ != 0 || detail_full_height_ != 0
        || detail_retained_bytes_ != 0;
    detail_full_width_ = 0;
    detail_full_height_ = 0;
    detail_retained_bytes_ = 0;
    if (had_geometry) {
        emit detailGeometryChanged();
    }
}

void EditController::applyState(BackendPhotoEditState state) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Catalog returned edit state for a different photo")));
        return;
    }
    setVersionDraft(state.is_version_draft);
    autosave_debounce_.stop();
    clearAutosaveFailure();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
    if (!version_draft_) {
        committed_grade_stack_ = state.grade_stack;
        durable_working_commit_id_ = state.base_commit_id;
    }
    base_commit_id_ = std::move(state.base_commit_id);
    setGradeStack(std::move(state.grade_stack));
    working_revision_ = 0;
    autosave_snapshot_revision_ = 0;
    clearSessionHistory();
    versions_.replace(std::move(state.versions));
}

void EditController::setGradeStack(
    BackendGradeStack grade_stack,
    const QString& preferred_grade_node_id
) {
    if (grade_stack.grade_nodes.size() > GradeNodeStack::maximum_grade_node_count) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "The saved edit exceeds the 16-Grade-Node desktop limit")));
        return;
    }
    const auto old_optics = grade_stack_.optics;
    const QString old_selected_id = selectedGradeNodeId();
    const int old_selected_index = selected_grade_node_index_;
    const BackendGradeNode* const old_selected = selectedGradeNode();
    const bool had_old_selection = old_selected != nullptr;
    const BackendGradeNode old_selected_value = had_old_selection
        ? *old_selected
        : BackendGradeNode{};
    const QString requested_id = preferred_grade_node_id.isEmpty()
        ? old_selected_id
        : preferred_grade_node_id;
    const int new_selected_index = GradeNodeStack::resolvedSelection(
        grade_stack,
        requested_id,
        old_selected_index
    );
    const BackendGradeNode* const new_selected = new_selected_index < 0
        ? nullptr
        : &grade_stack.grade_nodes.at(new_selected_index);
    const bool has_new_selection = new_selected != nullptr;
    const bool selection_changed = old_selected_index != new_selected_index
        || old_selected_id
            != (has_new_selection ? new_selected->grade_node_id : QString{});
    const bool grade_node_enabled_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.enabled != new_selected->enabled);
    const bool basic_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && (old_selected_value.basic != new_selected->basic
                || old_selected_value.fine != new_selected->fine));
    const bool curve_changed = selection_changed
        || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.fine.oklab_lightness_curve_points
                != new_selected->fine.oklab_lightness_curve_points);
    const bool list_changed = grade_node_list_changed(grade_stack_, grade_stack);
    const auto model_points = tone_curve_model_points(new_selected);
    if (tone_curve_points_.points() != model_points
        && !tone_curve_points_.replace(model_points)) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "The saved Tone Curve cannot be represented safely")));
        return;
    }
    grade_stack_ = std::move(grade_stack);
    selected_grade_node_index_ = new_selected_index;
    const int new_point_color_count = selectedGradeNode() == nullptr
        ? 0 : point_color_count(selectedGradeNode()->fine);
    selected_point_color_index_ = new_point_color_count == 0
        ? -1
        : selection_changed ? 0
                            : std::clamp(selected_point_color_index_, 0, new_point_color_count - 1);
    if (list_changed) {
        emit gradeNodesChanged();
    }
    if (selection_changed) {
        emit selectedGradeNodeChanged();
    }
    if (list_changed || selection_changed) {
        emit gradeNodeActionsChanged();
    }
    if (grade_node_enabled_changed) {
        emit gradeNodeEnabledChanged();
    }
    if (basic_changed) {
        notifyParametersChanged();
    }
    if (curve_changed) {
        emit toneCurveChanged();
    }
    if (old_optics != grade_stack_.optics) {
        emit opticsChanged();
    }
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
}

const BackendGradeNode* EditController::selectedGradeNode() const noexcept {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    if (selected_grade_node_index_ < 0 || selected_grade_node_index_ >= count) {
        return nullptr;
    }
    return &grade_stack_.grade_nodes.at(selected_grade_node_index_);
}

QString EditController::gradeNodeHistoryKey(const QString& key) const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr
        ? key
        : QStringLiteral("grade_node/%1/%2").arg(grade_node->grade_node_id, key);
}

QString EditController::uniqueGradeNodeLabel(const QString& base) const {
    const QString clean_base = base.trimmed().isEmpty()
        ? QStringLiteral("Adjustments")
        : base.trimmed();
    const auto exists = [this](const QString& candidate) {
        return std::any_of(
            grade_stack_.grade_nodes.cbegin(),
            grade_stack_.grade_nodes.cend(),
            [&candidate](const BackendGradeNode& grade_node) {
                return grade_node.label == candidate;
            }
        );
    };
    if (!exists(clean_base)) {
        return clean_base;
    }
    for (int suffix = 2; suffix <= GradeNodeStack::maximum_grade_node_count + 1; ++suffix) {
        const QString candidate = QStringLiteral("%1 %2").arg(clean_base).arg(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return clean_base + QStringLiteral(" Copy");
}

void EditController::finishActiveGesture() {
    active_parameter_gestures_.clear();
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.finishGesture(grade_stack_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::clearSessionHistory() {
    const bool had_history = history_.canUndo() || history_.canRedo();
    history_.clear();
    if (had_history) {
        emit historyChanged();
    }
}

void EditController::recordWorkingTransition(
    const QString& key,
    const BackendGradeStack& before
) {
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.record(key.toStdString(), before, grade_stack_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
    ++working_revision_;
    autosave_requested_ = true;
    clearAutosaveFailure();
    if (dirty_ && !state_running_) {
        scheduleAutosave();
    }
}

void EditController::schedulePreview(const int delay_ms) {
    if (!active_) {
        return;
    }
    // A newly edited Recipe makes an idle full-detail warmup useless. Advance
    // the shared request token before the preview work competes for CPU; the
    // prepared source itself remains reusable, but the old tile render exits
    // at its next cancellation boundary.
    if (detail_warmup_debounce_.isActive() || detail_warmup_watcher_.isRunning()) {
        detail_warmup_debounce_.stop();
        detail_warmup_token_ = backend_->beginEditDetailRequest();
    }
    ++render_revision_;
    markHistogramUpdating(EditPreviewKind::Current);
    if (detail_mode_) {
        invalidateDetailPresentation();
        detail_queued_ = true;
        detail_debounce_.start(std::max(delay_ms, EDIT_DETAIL_DEBOUNCE_MS));
    }
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
    }
    // A leading-edge throttle keeps the first response prompt during a drag.
    // Repeated slider events do not postpone that first frame indefinitely.
    if (delay_ms <= 0 || !preview_debounce_.isActive()) {
        preview_debounce_.start(delay_ms);
    }
}

bool EditController::eventFilter(QObject *const watched, QEvent *const event) {
  if (watched == QCoreApplication::instance() &&
      event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  return QObject::eventFilter(watched, event);
}

void EditController::retranslateUi() {
  emit statusTextChanged();
  if (!autosave_error_message_.isEmpty()) {
    emit autosaveErrorTextChanged();
  }
  emit gradeNodesChanged();
  if (!before_error_message_.isEmpty()) {
    emit beforeErrorTextChanged();
  }
  if (!detail_error_message_.isEmpty()) {
    emit detailErrorTextChanged();
  }
}

void EditController::setStatusMessage(LocalizedUiMessage status) {
    if (status_message_ == status) {
        return;
    }
  status_message_ = std::move(status);
    emit statusTextChanged();
}

void EditController::setDirty(const bool dirty) {
    if (dirty_ == dirty) {
        if (dirty && autosave_requested_ && !state_running_) {
            scheduleAutosave();
        }
        return;
    }
    dirty_ = dirty;
    emit dirtyChanged();
    if (dirty_ && autosave_requested_ && !state_running_) {
        scheduleAutosave();
    } else if (!dirty_) {
        autosave_debounce_.stop();
    }
}

void EditController::setAutosaveFailure(LocalizedUiMessage error) {
    if (autosave_error_message_ == error) {
        return;
    }
    autosave_error_message_ = std::move(error);
    emit autosaveFailedChanged();
    emit autosaveErrorTextChanged();
    emit autosavePendingChanged();
}

void EditController::clearAutosaveFailure() {
    if (autosave_error_message_.isEmpty()) {
        return;
    }
    autosave_error_message_.clear();
    emit autosaveFailedChanged();
    emit autosaveErrorTextChanged();
    emit autosavePendingChanged();
}

void EditController::scheduleAutosave() {
    if (!active_ || !dirty_ || !autosave_requested_ || state_running_) {
        return;
    }
    autosave_debounce_.start(EDIT_AUTOSAVE_DEBOUNCE_MS);
    emit autosavePendingChanged();
}

void EditController::startAutosave() {
    autosave_debounce_.stop();
    if (!active_ || !dirty_ || !autosave_requested_ || state_running_) {
        return;
    }
    clearAutosaveFailure();
    history_.finishGesture(grade_stack_);
    emit historyChanged();
    autosave_snapshot_revision_ = working_revision_;
    state_task_kind_ = EditStateTaskKind::Autosave;
    setStateRunning(true);
    emit autosavePendingChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Saving current adjustments locally…")));
    state_watcher_.setFuture(QtConcurrent::run(
        autosave_state,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        durable_working_commit_id_,
        grade_stack_,
        photo_generation_
    ));
}

bool EditController::applyAutosavedState(BackendPhotoEditState state) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Catalog returned autosave state for a different photo")));
        return false;
    }
    const bool changed_after_snapshot = working_revision_ != autosave_snapshot_revision_;
    const BackendGradeStack saved_stack = std::move(state.grade_stack);
    if (!changed_after_snapshot && grade_stack_ != saved_stack) {
        // The visible stack is the one that was persisted. A mismatch means
        // the Catalog had to canonicalize or recover it, so accept that
        // authoritative representation only when no newer local edit exists.
        setGradeStack(saved_stack);
    }
    setVersionDraft(false);
    clearAutosaveFailure();
    base_commit_id_ = state.base_commit_id;
    durable_working_commit_id_ = state.base_commit_id;
    committed_grade_stack_ = saved_stack;
    versions_.replace(std::move(state.versions));
    autosave_requested_ = changed_after_snapshot;
    setDirty(changed_after_snapshot);
    emit autosavePendingChanged();
    return changed_after_snapshot;
}

void EditController::setVersionDraft(const bool draft) {
    if (version_draft_ == draft) {
        return;
    }
    version_draft_ = draft;
    emit versionDraftChanged();
}

void EditController::setStateRunning(const bool running) {
    if (state_running_ == running) {
        return;
    }
    const bool previous_busy = busy();
    const bool previously_locked = interactionLocked();
    state_running_ = running;
    if (!running) {
        state_task_kind_ = EditStateTaskKind::Open;
    }
    if (previously_locked != interactionLocked()) {
        emit stateBusyChanged();
    }
    emit autosavePendingChanged();
    emit gradeNodeActionsChanged();
    emitBusyChange(previous_busy);
}

void EditController::setPreviewRunning(
    const EditPreviewKind kind,
    const bool running
) {
    const bool previous_busy = busy();
    if (kind == EditPreviewKind::Current) {
        if (current_rendering_ == running) {
            return;
        }
        current_rendering_ = running;
        emit renderingChanged();
    } else {
        if (before_rendering_ == running) {
            return;
        }
        before_rendering_ = running;
        emit beforeRenderingChanged();
    }
    emitBusyChange(previous_busy);
}

void EditController::markHistogramUpdating(const EditPreviewKind kind) {
    QVariantMap& target = kind == EditPreviewKind::Current
        ? histogram_ : before_histogram_;
    target.insert(QStringLiteral("updating"), true);
    target.insert(QStringLiteral("stale"), false);
    target.insert(
        QStringLiteral("targetGeneration"),
        QVariant::fromValue<qulonglong>(
            kind == EditPreviewKind::Current ? render_revision_ : photo_generation_
        )
    );
    if (kind == EditPreviewKind::Current) {
        emit histogramChanged();
    } else {
        emit beforeHistogramChanged();
    }
}

void EditController::publishHistogram(
    const EditPreviewKind kind,
    const BackendEditPreviewAnalysis& analysis,
    const quint64 generation
) {
    QVariantMap snapshot = histogram_snapshot(analysis, generation);
    if (kind == EditPreviewKind::Current) {
        histogram_ = std::move(snapshot);
        emit histogramChanged();
    } else {
        before_histogram_ = std::move(snapshot);
        emit beforeHistogramChanged();
    }
}

void EditController::markHistogramFailed(const EditPreviewKind kind) {
    QVariantMap& target = kind == EditPreviewKind::Current
        ? histogram_ : before_histogram_;
    target.insert(QStringLiteral("updating"), false);
    target.insert(QStringLiteral("stale"), true);
    if (kind == EditPreviewKind::Current) {
        emit histogramChanged();
    } else {
        emit beforeHistogramChanged();
    }
}

void EditController::clearHistograms() {
    const QVariantMap empty = empty_histogram();
    if (histogram_ != empty) {
        histogram_ = empty;
        emit histogramChanged();
    }
    if (before_histogram_ != empty) {
        before_histogram_ = empty;
        emit beforeHistogramChanged();
    }
}

void EditController::setDetailRunning(const bool running) {
    if (detail_rendering_ == running) {
        return;
    }
    const bool previous_busy = busy();
    detail_rendering_ = running;
    emit detailRenderingChanged();
    emitBusyChange(previous_busy);
}

void EditController::emitBusyChange(const bool previous_busy) {
    if (previous_busy != busy()) {
        emit busyChanged();
    }
}

void EditController::parameterEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    if (!active_ || interactionLocked()) {
        return;
    }
    recordWorkingTransition(gradeNodeHistoryKey(key), before);
    notifyParametersChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(EDIT_PREVIEW_THROTTLE_MS);
}

void EditController::opticsEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    recordWorkingTransition(QStringLiteral("optics/%1").arg(key), before);
    emit opticsChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    // Geometry remapping is materially more expensive than a scalar Grade
    // adjustment. Coalesce slider samples to one interactive frame instead
    // of scheduling a separate complete render for every mouse move.
    schedulePreview(EDIT_PREVIEW_THROTTLE_MS);
}

void EditController::notifyParametersChanged() {
    ++parameter_revision_;
    emit parametersChanged();
}

void EditController::toneCurveEdited(
    const QString& key,
    const BackendGradeStack& before,
    const int preview_delay_ms
) {
    recordWorkingTransition(gradeNodeHistoryKey(key), before);
    emit toneCurveChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(preview_delay_ms);
}

bool EditController::acceptParameter(
    const double value,
    const double minimum,
    const double maximum,
    const char *const label_source) {
    if (!active_ || interactionLocked()) {
        return false;
    }
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Select a Grade Node before editing")));
        return false;
    }
    if (!grade_node->enabled) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Enable the selected Grade Node before editing its controls")));
        return false;
    }
    if (!std::isfinite(value) || value < minimum || value > maximum) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController",
                          "%1 is outside the supported preview range"),
        {LocalizedUiArgument::translatedText("EditController", label_source)})
        );
        return false;
    }
    return true;
}
