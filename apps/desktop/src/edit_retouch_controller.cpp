#include "edit_controller.hpp"
#include "edit_retouch_donor_selection.hpp"
#include "edit_stroke_input.hpp"

#include <algorithm>

#include <array>
#include <cmath>
#include <initializer_list>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] LocalizedUiMessage retouch_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] std::pair<double, double>
default_retouch_source_offset(const double normalized_x, const double normalized_y) {
    return {
        normalized_x <= 0.5 ? 3.0 : -3.0,
        normalized_y <= 0.5 ? 1.5 : -1.5,
    };
}

[[nodiscard]] std::pair<double, double>
display_retouch_spot_source_offset(const BackendRetouchSpot& spot) {
    if (spot.source_offset_x_radii != 0.0 || spot.source_offset_y_radii != 0.0) {
        return {spot.source_offset_x_radii, spot.source_offset_y_radii};
    }
    return default_retouch_source_offset(spot.center_x, spot.center_y);
}

[[nodiscard]] std::pair<double, double>
display_retouch_stroke_source_offset(const BackendRetouchStroke& stroke) {
    if (stroke.source_offset_x_radii != 0.0 || stroke.source_offset_y_radii != 0.0) {
        return {stroke.source_offset_x_radii, stroke.source_offset_y_radii};
    }
    double lower_x = 1.0;
    double upper_x = 0.0;
    double lower_y = 1.0;
    double upper_y = 0.0;
    for (const auto& point : stroke.points) {
        lower_x = std::min(lower_x, point.x);
        upper_x = std::max(upper_x, point.x);
        lower_y = std::min(lower_y, point.y);
        upper_y = std::max(upper_y, point.y);
    }
    const double center_x = std::midpoint(lower_x, upper_x);
    const double center_y = std::midpoint(lower_y, upper_y);
    if (upper_x - lower_x >= upper_y - lower_y) {
        return {0.0, center_y <= 0.5 ? 3.0 : -3.0};
    }
    return {center_x <= 0.5 ? 3.0 : -3.0, 0.0};
}

[[nodiscard]] std::optional<QPointF> preview_retouch_source_offset(
    const std::shared_ptr<EditPreviewStore>& preview_store,
    const QString& preview_generation,
    const QSize level_zero_dimensions,
    const std::span<const QPointF> points,
    const int creation_mode
) {
    bool valid_generation = false;
    const quint64 generation = preview_generation.toULongLong(&valid_generation);
    if (!valid_generation || preview_store == nullptr) {
        return std::nullopt;
    }
    const EditPreviewStore::Snapshot snapshot =
        preview_store->snapshot(EditPreviewSlot::Current, generation);
    std::span<const std::uint8_t> pixels;
    try {
        if (snapshot.frame != nullptr) {
            pixels = snapshot.frame->materializeRgb8();
        } else if (!snapshot.bytes.isEmpty()) {
            pixels = {
                reinterpret_cast<const std::uint8_t*>(snapshot.bytes.constData()),
                static_cast<std::size_t>(snapshot.bytes.size()),
            };
        }
    } catch (...) {
        return std::nullopt;
    }
    if (snapshot.row_stride_bytes <= 0) {
        return std::nullopt;
    }
    return select_edit_retouch_donor_offset({
        .preview_rgb8 = pixels,
        .preview_dimensions = snapshot.dimensions,
        .preview_row_stride_bytes = static_cast<std::size_t>(snapshot.row_stride_bytes),
        .level_zero_dimensions = level_zero_dimensions,
        .normalized_target_points = points,
        .radius_level_zero_pixels = 18.0,
        .mode = creation_mode == 1 ? EditRetouchDonorMode::Clone : EditRetouchDonorMode::Heal,
    });
}

} // namespace

QVariantList EditController::retouchSpots() const {
    QVariantList result;
    result.reserve(grade_stack_.retouch_spots.size());
    for (qsizetype index = 0; index < grade_stack_.retouch_spots.size(); ++index) {
        const auto& spot = grade_stack_.retouch_spots.at(index);
        const auto [source_offset_x, source_offset_y] = display_retouch_spot_source_offset(spot);
        result.push_back(
            QVariantMap{
                {QStringLiteral("index"), static_cast<int>(index)},
                {QStringLiteral("x"), spot.center_x},
                {QStringLiteral("y"), spot.center_y},
                {QStringLiteral("radius"), static_cast<int>(spot.radius_level_zero_pixels)},
                {QStringLiteral("mode"), static_cast<int>(spot.mode)},
                {QStringLiteral("sourceOffsetX"), source_offset_x},
                {QStringLiteral("sourceOffsetY"), source_offset_y},
                {QStringLiteral("feather"), spot.feather},
                {QStringLiteral("strength"), spot.strength},
            }
        );
    }
    return result;
}

QVariantList EditController::retouchStrokes() const {
    QVariantList result;
    result.reserve(grade_stack_.retouch_strokes.size());
    for (qsizetype index = 0; index < grade_stack_.retouch_strokes.size(); ++index) {
        const auto& stroke = grade_stack_.retouch_strokes.at(index);
        QVariantList points;
        points.reserve(stroke.points.size());
        for (const auto& point : stroke.points) {
            points.push_back(
                QVariantMap{
                    {QStringLiteral("x"), point.x},
                    {QStringLiteral("y"), point.y},
                }
            );
        }
        const auto [source_offset_x, source_offset_y] =
            display_retouch_stroke_source_offset(stroke);
        result.push_back(
            QVariantMap{
                {QStringLiteral("index"), static_cast<int>(index)},
                {QStringLiteral("points"), points},
                {QStringLiteral("radius"), static_cast<int>(stroke.radius_level_zero_pixels)},
                {QStringLiteral("mode"), static_cast<int>(stroke.mode)},
                {QStringLiteral("sourceOffsetX"), source_offset_x},
                {QStringLiteral("sourceOffsetY"), source_offset_y},
                {QStringLiteral("feather"), stroke.feather},
                {QStringLiteral("strength"), stroke.strength},
            }
        );
    }
    return result;
}

bool EditController::retouchPickerActive() const noexcept {
    return retouch_picker_active_;
}

int EditController::retouchCreationMode() const noexcept {
    return retouch_creation_mode_;
}

void EditController::setRetouchPickerActive(const bool active) {
    if (retouch_picker_active_ == active) {
        return;
    }
    retouch_picker_active_ = active;
    emit retouchPickerActiveChanged();
    if (active && point_color_picker_active_) {
        point_color_picker_active_ = false;
        emit pointColorPickerActiveChanged();
    }
    if (active && white_balance_picker_active_) {
        white_balance_picker_active_ = false;
        emit whiteBalancePickerActiveChanged();
    }
}

void EditController::setRetouchCreationMode(const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if ((mode != heal_mode && mode != clone_mode) || retouch_creation_mode_ == mode) {
        return;
    }
    retouch_creation_mode_ = mode;
    emit retouchCreationModeChanged();
}

void EditController::addRetouchSpotFromPreview(
    const double normalized_x,
    const double normalized_y,
    const QString& preview_generation,
    const int level_zero_width,
    const int level_zero_height
) {
    if (!active_ || interactionLocked() || !retouch_picker_active_ || !std::isfinite(normalized_x)
        || !std::isfinite(normalized_y) || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    constexpr qsizetype maximum_retouch_spots = 64;
    if (grade_stack_.retouch_spots.size() >= maximum_retouch_spots) {
        setStatusMessage(
            retouch_message(QT_TRANSLATE_NOOP("EditController", "Repair supports at most 64 spots"))
        );
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const std::array<QPointF, 1U> target_points{
        QPointF(normalized_x, normalized_y),
    };
    const std::optional<QPointF> selected_source = preview_retouch_source_offset(
        preview_store_,
        preview_generation,
        QSize(level_zero_width, level_zero_height),
        target_points,
        retouch_creation_mode_
    );
    const auto fallback_source = retouch_creation_mode_ == 1
                                     ? default_retouch_source_offset(normalized_x, normalized_y)
                                     : std::pair<double, double>{0.0, 0.0};
    const double source_offset_x =
        selected_source.has_value() ? selected_source->x() : fallback_source.first;
    const double source_offset_y =
        selected_source.has_value() ? selected_source->y() : fallback_source.second;
    grade_stack_.retouch_spots.push_back(
        BackendRetouchSpot{
            .center_x = normalized_x,
            .center_y = normalized_y,
            .radius_level_zero_pixels = 18U,
            .mode = static_cast<std::uint8_t>(retouch_creation_mode_),
            .source_offset_x_radii = source_offset_x,
            .source_offset_y_radii = source_offset_y,
            .feather = 0.28,
            .strength = 1.0,
        }
    );
    parameterEdited(QStringLiteral("retouch/add"), before);
    setStatusMessage(retouch_message(QT_TRANSLATE_NOOP("EditController", "Added repair spot")));
}

void EditController::addRetouchStrokeFromPreview(
    const QVariantList& points,
    const QString& preview_generation,
    const int level_zero_width,
    const int level_zero_height
) {
    constexpr qsizetype maximum_retouch_strokes = 64;
    constexpr qsizetype maximum_retouch_stroke_points = 512;
    if (!active_ || interactionLocked() || !retouch_picker_active_
        || grade_stack_.retouch_strokes.size() >= maximum_retouch_strokes) {
        if (grade_stack_.retouch_strokes.size() >= maximum_retouch_strokes) {
            setStatusMessage(retouch_message(
                QT_TRANSLATE_NOOP("EditController", "Repair supports at most 64 strokes")
            ));
        }
        return;
    }
    const auto normalized_points =
        EditStrokeInput::decodeNormalizedPoints(points, maximum_retouch_stroke_points);
    if (!normalized_points.has_value()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    QVector<BackendRetouchStrokePoint> stroke_points;
    stroke_points.reserve(normalized_points->size());
    for (const QPointF& point : *normalized_points) {
        stroke_points.push_back({.x = point.x(), .y = point.y()});
    }
    const QPointF first_point = normalized_points->constFirst();
    const std::optional<QPointF> selected_source = preview_retouch_source_offset(
        preview_store_,
        preview_generation,
        QSize(level_zero_width, level_zero_height),
        std::span<const QPointF>(
            normalized_points->constData(),
            static_cast<std::size_t>(normalized_points->size())
        ),
        retouch_creation_mode_
    );
    const auto fallback_source =
        retouch_creation_mode_ == 1
            ? default_retouch_source_offset(first_point.x(), first_point.y())
            : std::pair<double, double>{0.0, 0.0};
    const double source_offset_x =
        selected_source.has_value() ? selected_source->x() : fallback_source.first;
    const double source_offset_y =
        selected_source.has_value() ? selected_source->y() : fallback_source.second;
    grade_stack_.retouch_strokes.push_back(
        BackendRetouchStroke{
            .points = std::move(stroke_points),
            .radius_level_zero_pixels = 18U,
            .mode = static_cast<std::uint8_t>(retouch_creation_mode_),
            .source_offset_x_radii = source_offset_x,
            .source_offset_y_radii = source_offset_y,
            .feather = 0.28,
            .strength = 1.0,
        }
    );
    parameterEdited(QStringLiteral("retouch/stroke/add"), before);
}

void EditController::setRetouchSpotCenter(
    const int index,
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(normalized_x) || !std::isfinite(normalized_y) || normalized_x < 0.0
        || normalized_x > 1.0 || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.center_x == normalized_x && spot.center_y == normalized_y) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.center_x = normalized_x;
    spot.center_y = normalized_y;
    parameterEdited(QStringLiteral("retouch/%1/center").arg(index), before);
}

void EditController::setRetouchSpotRadius(const int index, const int radius_level_zero_pixels) {
    constexpr int minimum_radius = 1;
    constexpr int maximum_radius = 128;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || radius_level_zero_pixels < minimum_radius || radius_level_zero_pixels > maximum_radius) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    const auto radius = static_cast<std::uint16_t>(radius_level_zero_pixels);
    if (spot.radius_level_zero_pixels == radius) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.radius_level_zero_pixels = radius;
    parameterEdited(QStringLiteral("retouch/%1/radius").arg(index), before);
}

void EditController::setRetouchSpotMode(const int index, const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || (mode != heal_mode && mode != clone_mode)) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (spot.mode == encoded_mode) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.mode = encoded_mode;
    if (mode == clone_mode && spot.source_offset_x_radii == 0.0
        && spot.source_offset_y_radii == 0.0) {
        const auto [source_offset_x, source_offset_y] =
            default_retouch_source_offset(spot.center_x, spot.center_y);
        spot.source_offset_x_radii = source_offset_x;
        spot.source_offset_y_radii = source_offset_y;
    }
    parameterEdited(QStringLiteral("retouch/%1/mode").arg(index), before);
}

void EditController::setRetouchSpotFeather(const int index, const double feather) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(feather) || feather < 0.0 || feather > 1.0) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.feather == feather) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.feather = feather;
    parameterEdited(QStringLiteral("retouch/%1/feather").arg(index), before);
}

void EditController::setRetouchSpotStrength(const int index, const double strength) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.strength == strength) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.strength = strength;
    parameterEdited(QStringLiteral("retouch/%1/strength").arg(index), before);
}

void EditController::setRetouchSpotSourceOffset(
    const int index,
    const double offset_x_radii,
    const double offset_y_radii
) {
    constexpr double maximum_offset_radii = 8.0;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(offset_x_radii) || !std::isfinite(offset_y_radii)
        || offset_x_radii < -maximum_offset_radii || offset_x_radii > maximum_offset_radii
        || offset_y_radii < -maximum_offset_radii || offset_y_radii > maximum_offset_radii) {
        return;
    }
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.source_offset_x_radii == offset_x_radii
        && spot.source_offset_y_radii == offset_y_radii) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    spot.source_offset_x_radii = offset_x_radii;
    spot.source_offset_y_radii = offset_y_radii;
    parameterEdited(QStringLiteral("retouch/%1/source").arg(index), before);
}

void EditController::removeRetouchSpot(const int index) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_spots.size()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_spots.removeAt(index);
    parameterEdited(QStringLiteral("retouch/remove"), before);
    setStatusMessage(retouch_message(QT_TRANSLATE_NOOP("EditController", "Removed repair spot")));
}

void EditController::setRetouchStrokeRadius(const int index, const int radius_level_zero_pixels) {
    constexpr int minimum_radius = 1;
    constexpr int maximum_radius = 128;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || radius_level_zero_pixels < minimum_radius || radius_level_zero_pixels > maximum_radius) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    const auto radius = static_cast<std::uint16_t>(radius_level_zero_pixels);
    if (stroke.radius_level_zero_pixels == radius) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.radius_level_zero_pixels = radius;
    parameterEdited(QStringLiteral("retouch/stroke/%1/radius").arg(index), before);
}

void EditController::setRetouchStrokeMode(const int index, const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || (mode != heal_mode && mode != clone_mode)) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (stroke.mode == encoded_mode) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.mode = encoded_mode;
    if (mode == clone_mode && stroke.source_offset_x_radii == 0.0
        && stroke.source_offset_y_radii == 0.0) {
        const BackendRetouchStrokePoint source_center =
            stroke.points.isEmpty() ? BackendRetouchStrokePoint{.x = 0.5, .y = 0.5}
                                    : stroke.points.front();
        const auto [source_offset_x, source_offset_y] =
            default_retouch_source_offset(source_center.x, source_center.y);
        stroke.source_offset_x_radii = source_offset_x;
        stroke.source_offset_y_radii = source_offset_y;
    }
    parameterEdited(QStringLiteral("retouch/stroke/%1/mode").arg(index), before);
}

void EditController::setRetouchStrokeFeather(const int index, const double feather) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(feather) || feather < 0.0 || feather > 1.0) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.feather == feather) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.feather = feather;
    parameterEdited(QStringLiteral("retouch/stroke/%1/feather").arg(index), before);
}

void EditController::setRetouchStrokeStrength(const int index, const double strength) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.strength == strength) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.strength = strength;
    parameterEdited(QStringLiteral("retouch/stroke/%1/strength").arg(index), before);
}

void EditController::setRetouchStrokeSourceOffset(
    const int index,
    const double offset_x_radii,
    const double offset_y_radii
) {
    constexpr double maximum_offset_radii = 8.0;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(offset_x_radii) || !std::isfinite(offset_y_radii)
        || offset_x_radii < -maximum_offset_radii || offset_x_radii > maximum_offset_radii
        || offset_y_radii < -maximum_offset_radii || offset_y_radii > maximum_offset_radii) {
        return;
    }
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.source_offset_x_radii == offset_x_radii
        && stroke.source_offset_y_radii == offset_y_radii) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    stroke.source_offset_x_radii = offset_x_radii;
    stroke.source_offset_y_radii = offset_y_radii;
    parameterEdited(QStringLiteral("retouch/stroke/%1/source").arg(index), before);
}

void EditController::removeRetouchStroke(const int index) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.retouch_strokes.size()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_strokes.removeAt(index);
    parameterEdited(QStringLiteral("retouch/stroke/remove"), before);
}

void EditController::clearRetouch() {
    if (!active_ || interactionLocked()
        || (grade_stack_.retouch_spots.isEmpty() && grade_stack_.retouch_strokes.isEmpty())) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_spots.clear();
    grade_stack_.retouch_strokes.clear();
    parameterEdited(QStringLiteral("retouch/reset"), before);
}
