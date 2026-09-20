#include "edit_controller.hpp"
#include "edit_retouch_donor_preview.hpp"
#include "edit_retouch_sources.hpp"
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

[[nodiscard]] std::pair<double, double> default_retouch_source_offset(
    const double normalized_x,
    const double normalized_y,
    const std::uint16_t radius_level_zero_pixels
) {
    constexpr double maximum_detail_apron_level_zero_pixels = 512.0;
    const double maximum_offset = (maximum_detail_apron_level_zero_pixels - 1.0)
                                      / static_cast<double>(radius_level_zero_pixels)
                                  - 1.0;
    const double horizontal = std::min(3.0, maximum_offset);
    const double vertical = std::min(1.5, maximum_offset);
    return {
        normalized_x <= 0.5 ? horizontal : -horizontal,
        normalized_y <= 0.5 ? vertical : -vertical,
    };
}

[[nodiscard]] bool retouch_source_offset_fits_detail_apron(
    const std::uint16_t radius_level_zero_pixels,
    const double horizontal,
    const double vertical,
    const double source_scale
) noexcept {
    constexpr double maximum_detail_apron_level_zero_pixels = 512.0;
    const double maximum = (maximum_detail_apron_level_zero_pixels - 1.0)
                               / static_cast<double>(radius_level_zero_pixels)
                           - source_scale;
    return std::abs(horizontal) <= maximum && std::abs(vertical) <= maximum;
}

void clamp_retouch_source_offset_to_detail_apron(
    const std::uint16_t radius_level_zero_pixels,
    double& horizontal,
    double& vertical,
    const double source_scale
) noexcept {
    constexpr double maximum_detail_apron_level_zero_pixels = 512.0;
    const double maximum = (maximum_detail_apron_level_zero_pixels - 1.0)
                               / static_cast<double>(radius_level_zero_pixels)
                           - source_scale;
    horizontal = std::clamp(horizontal, -maximum, maximum);
    vertical = std::clamp(vertical, -maximum, maximum);
}

[[nodiscard]] std::pair<double, double>
display_retouch_spot_source_offset(const BackendRetouchSpot& spot) {
    if (spot.source_offset_x_radii != 0.0 || spot.source_offset_y_radii != 0.0) {
        return {spot.source_offset_x_radii, spot.source_offset_y_radii};
    }
    return default_retouch_source_offset(
        spot.center_x,
        spot.center_y,
        spot.radius_level_zero_pixels
    );
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
    const double automatic_distance =
        std::min(3.0, (512.0 - 1.0) / static_cast<double>(stroke.radius_level_zero_pixels) - 1.0);
    if (upper_x - lower_x >= upper_y - lower_y) {
        return {0.0, center_y <= 0.5 ? automatic_distance : -automatic_distance};
    }
    return {center_x <= 0.5 ? automatic_distance : -automatic_distance, 0.0};
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
                {QStringLiteral("sourceRotation"), spot.source_rotation_degrees},
                {QStringLiteral("sourceScale"), spot.source_scale},
                {QStringLiteral("sourceFlipHorizontal"), spot.source_flip_horizontal},
                {QStringLiteral("sourceFlipVertical"), spot.source_flip_vertical},
                {QStringLiteral("feather"), spot.feather},
                {QStringLiteral("strength"), spot.strength},
                {QStringLiteral("frequencyRadius"), int(spot.frequency_radius)},
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
                {QStringLiteral("sourceRotation"), stroke.source_rotation_degrees},
                {QStringLiteral("sourceScale"), stroke.source_scale},
                {QStringLiteral("sourceFlipHorizontal"), stroke.source_flip_horizontal},
                {QStringLiteral("sourceFlipVertical"), stroke.source_flip_vertical},
                {QStringLiteral("feather"), stroke.feather},
                {QStringLiteral("strength"), stroke.strength},
                {QStringLiteral("frequencyRadius"), int(stroke.frequency_radius)},
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

int EditController::retouchBrushRadius() const noexcept {
    return retouch_brush_radius_;
}

double EditController::retouchBrushFeather() const noexcept {
    return retouch_brush_feather_;
}

double EditController::retouchBrushStrength() const noexcept {
    return retouch_brush_strength_;
}

bool EditController::retouchSourceAligned() const noexcept {
    return retouch_source_aligned_;
}

bool EditController::retouchSourcePicking() const noexcept {
    return retouch_source_picking_;
}

bool EditController::retouchSourceSampled() const noexcept {
    return retouch_source_anchor_.has_value();
}

QVariantMap EditController::retouchSampledSource() const {
    if (!retouch_source_anchor_.has_value()) {
        return {};
    }
    return {
        {QStringLiteral("x"), retouch_source_anchor_->x()},
        {QStringLiteral("y"), retouch_source_anchor_->y()},
    };
}

void EditController::setRetouchPickerActive(const bool active) {
    if (retouch_picker_active_ == active) {
        return;
    }
    retouch_picker_active_ = active;
    emit retouchPickerActiveChanged();
    if (!active
        && (retouch_source_picking_ || retouch_source_anchor_.has_value()
            || retouch_aligned_source_offset_radii_.has_value())) {
        retouch_source_picking_ = false;
        retouch_source_anchor_.reset();
        retouch_aligned_source_offset_radii_.reset();
        emit retouchSourceChanged();
    }
    if (active && point_color_picker_active_) {
        point_color_picker_active_ = false;
        emit pointColorPickerActiveChanged();
    }
    if (active && white_balance_picker_active_) {
        white_balance_picker_active_ = false;
        emit whiteBalancePickerActiveChanged();
    }
    if (active && raw_white_balance_picker_active_) {
        raw_white_balance_picker_active_ = false;
        emit rawWhiteBalancePickerActiveChanged();
    }
}

void EditController::setRetouchCreationMode(const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    if ((mode != heal_mode && mode != clone_mode && mode != 3 && mode != 4)
        || retouch_creation_mode_ == mode) {
        return;
    }
    retouch_creation_mode_ = mode;
    emit retouchCreationModeChanged();
    if (mode == heal_mode && retouch_source_picking_) {
        retouch_source_picking_ = false;
        emit retouchSourceChanged();
    }
}

void EditController::setRetouchBrushRadius(const int radius_level_zero_pixels) {
    constexpr int minimum_radius = 1;
    constexpr int maximum_radius = 128;
    const int radius = std::clamp(radius_level_zero_pixels, minimum_radius, maximum_radius);
    if (retouch_brush_radius_ == radius) {
        return;
    }
    retouch_brush_radius_ = radius;
    emit retouchBrushChanged();
}

void EditController::setRetouchBrushFeather(const double feather) {
    if (!std::isfinite(feather)) {
        return;
    }
    const double bounded = std::clamp(feather, 0.0, 1.0);
    if (retouch_brush_feather_ == bounded) {
        return;
    }
    retouch_brush_feather_ = bounded;
    emit retouchBrushChanged();
}

void EditController::setRetouchBrushStrength(const double strength) {
    if (!std::isfinite(strength)) {
        return;
    }
    const double bounded = std::clamp(strength, 0.0, 1.0);
    if (retouch_brush_strength_ == bounded) {
        return;
    }
    retouch_brush_strength_ = bounded;
    emit retouchBrushChanged();
}

void EditController::adjustRetouchBrushRadius(const int direction) {
    if (direction == 0) {
        return;
    }
    const int step = retouch_brush_radius_ < 20 ? 1 : retouch_brush_radius_ < 64 ? 2 : 4;
    setRetouchBrushRadius(retouch_brush_radius_ + (direction < 0 ? -step : step));
}

void EditController::setRetouchSourceAligned(const bool aligned) {
    if (retouch_source_aligned_ == aligned) {
        return;
    }
    retouch_source_aligned_ = aligned;
    retouch_aligned_source_offset_radii_.reset();
    emit retouchSourceChanged();
}

void EditController::setRetouchSourcePicking(const bool picking) {
    const bool bounded = picking && active_ && !interactionLocked() && retouch_picker_active_;
    if (retouch_source_picking_ == bounded) {
        return;
    }
    retouch_source_picking_ = bounded;
    emit retouchSourceChanged();
    if (bounded) {
        setStatusMessage(retouch_message(
            QT_TRANSLATE_NOOP("EditController", "Click the image to choose a repair source")
        ));
    }
}

void EditController::setRetouchSourceFromPreview(
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || !retouch_picker_active_ || !std::isfinite(normalized_x)
        || !std::isfinite(normalized_y) || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    retouch_source_anchor_ = QPointF(normalized_x, normalized_y);
    retouch_source_picking_ = false;
    retouch_aligned_source_offset_radii_.reset();
    emit retouchSourceChanged();
    setStatusMessage(retouch_message(QT_TRANSLATE_NOOP("EditController", "Repair source sampled")));
}

void EditController::moveRetouchSourceFromPreview(
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || !retouch_picker_active_
        || !retouch_source_anchor_.has_value() || !std::isfinite(normalized_x)
        || !std::isfinite(normalized_y) || normalized_x < 0.0 || normalized_x > 1.0
        || normalized_y < 0.0 || normalized_y > 1.0) {
        return;
    }
    const QPointF next_anchor(normalized_x, normalized_y);
    if (*retouch_source_anchor_ == next_anchor) {
        return;
    }
    retouch_source_anchor_ = next_anchor;
    retouch_aligned_source_offset_radii_.reset();
    emit retouchSourceChanged();
}

void EditController::clearRetouchSource() {
    constexpr int clone_mode = 1;
    const bool next_picking = retouch_picker_active_ && retouch_creation_mode_ == clone_mode;
    if (!retouch_source_anchor_.has_value() && !retouch_aligned_source_offset_radii_.has_value()
        && retouch_source_picking_ == next_picking) {
        return;
    }
    retouch_source_anchor_.reset();
    retouch_aligned_source_offset_radii_.reset();
    retouch_source_picking_ = next_picking;
    emit retouchSourceChanged();
}

bool EditController::retouchNodeEnabled() const noexcept {
    return (!grade_stack_.retouch_spots.isEmpty() || !grade_stack_.retouch_strokes.isEmpty())
           && grade_stack_.retouch_enabled;
}

void EditController::setRetouchNodeEnabled(const bool enabled) {
    if (!active_ || interactionLocked()
        || (grade_stack_.retouch_spots.isEmpty() && grade_stack_.retouch_strokes.isEmpty())
        || grade_stack_.retouch_enabled == enabled) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_enabled = enabled;
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("retouch/enabled"), before);
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
    constexpr int clone_mode = 1;
    if (retouch_creation_mode_ == clone_mode && !retouch_source_anchor_.has_value()) {
        if (!retouch_source_picking_) {
            retouch_source_picking_ = true;
            emit retouchSourceChanged();
        }
        setStatusMessage(retouch_message(
            QT_TRANSLATE_NOOP("EditController", "Choose a clone source before painting")
        ));
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
    grade_stack_.retouch_enabled = true;
    const std::array<QPointF, 1U> target_points{
        QPointF(normalized_x, normalized_y),
    };
    const auto creation_radius = static_cast<std::uint16_t>(retouch_brush_radius_);
    std::optional<QPointF> selected_source;
    std::optional<EditRetouchDonorSelection> automatic_selection;
    const bool sampled_source = retouch_source_anchor_.has_value();
    if (sampled_source) {
        selected_source =
            retouch_source_aligned_ && retouch_aligned_source_offset_radii_.has_value()
                ? retouch_aligned_source_offset_radii_
                : std::optional<QPointF>{QPointF(
                      (retouch_source_anchor_->x() - normalized_x)
                          * static_cast<double>(level_zero_width) / creation_radius,
                      (retouch_source_anchor_->y() - normalized_y)
                          * static_cast<double>(level_zero_height) / creation_radius
                  )};
    } else if (retouch_creation_mode_ != clone_mode) {
        automatic_selection = preview_retouch_source_selection(
            preview_store_,
            preview_generation,
            QSize(level_zero_width, level_zero_height),
            target_points,
            static_cast<double>(creation_radius),
            retouch_creation_mode_
        );
        if (automatic_selection.has_value()) {
            selected_source = automatic_selection->offset_radii;
        }
    }
    const auto fallback_source =
        retouch_creation_mode_ == 1
            ? default_retouch_source_offset(normalized_x, normalized_y, creation_radius)
            : std::pair<double, double>{0.0, 0.0};
    const double source_offset_x =
        selected_source.has_value() ? selected_source->x() : fallback_source.first;
    const double source_offset_y =
        selected_source.has_value() ? selected_source->y() : fallback_source.second;
    if (!retouch_source_offset_fits_detail_apron(
            creation_radius,
            source_offset_x,
            source_offset_y,
            1.0
        )) {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Sampled source is farther than the 512 px detail limit"
        )));
        return;
    }
    grade_stack_.retouch_spots.push_back(
        BackendRetouchSpot{
            .center_x = normalized_x,
            .center_y = normalized_y,
            .radius_level_zero_pixels = creation_radius,
            .mode = static_cast<std::uint8_t>(retouch_creation_mode_),
            .source_offset_x_radii = source_offset_x,
            .source_offset_y_radii = source_offset_y,
            .feather = retouch_brush_feather_,
            .strength = retouch_brush_strength_,
            .frequency_radius = static_cast<std::uint16_t>(retouch_sources_->frequencyRadius()),
        }
    );
    if (sampled_source && retouch_source_aligned_
        && !retouch_aligned_source_offset_radii_.has_value()) {
        retouch_aligned_source_offset_radii_ = QPointF(source_offset_x, source_offset_y);
        emit retouchSourceChanged();
    }
    parameterEdited(QStringLiteral("retouch/add"), before);
    if (!sampled_source && !automatic_selection.has_value()) {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Using a nearby fallback source · drag the outlined source to refine it"
        )));
    } else if (automatic_selection.has_value() && automatic_selection->confidence < 0.38) {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Automatic repair source is uncertain · drag the outlined source to refine it"
        )));
    } else {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP("EditController", "Added repair spot")));
    }
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
    constexpr int clone_mode = 1;
    if (retouch_creation_mode_ == clone_mode && !retouch_source_anchor_.has_value()) {
        if (!retouch_source_picking_) {
            retouch_source_picking_ = true;
            emit retouchSourceChanged();
        }
        setStatusMessage(retouch_message(
            QT_TRANSLATE_NOOP("EditController", "Choose a clone source before painting")
        ));
        return;
    }
    const auto normalized_points =
        EditStrokeInput::decodeNormalizedPoints(points, maximum_retouch_stroke_points);
    if (!normalized_points.has_value()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.retouch_enabled = true;
    QVector<BackendRetouchStrokePoint> stroke_points;
    stroke_points.reserve(normalized_points->size());
    for (const QPointF& point : *normalized_points) {
        stroke_points.push_back({.x = point.x(), .y = point.y()});
    }
    const auto target_anchor = EditStrokeInput::normalizedBoundsCenter(
        std::span<const QPointF>(
            normalized_points->constData(),
            static_cast<std::size_t>(normalized_points->size())
        )
    );
    if (!target_anchor.has_value()) {
        return;
    }
    const auto creation_radius = static_cast<std::uint16_t>(retouch_brush_radius_);
    std::optional<QPointF> selected_source;
    std::optional<EditRetouchDonorSelection> automatic_selection;
    const bool sampled_source = retouch_source_anchor_.has_value();
    if (sampled_source) {
        selected_source =
            retouch_source_aligned_ && retouch_aligned_source_offset_radii_.has_value()
                ? retouch_aligned_source_offset_radii_
                : std::optional<QPointF>{QPointF(
                      (retouch_source_anchor_->x() - target_anchor->x())
                          * static_cast<double>(level_zero_width) / creation_radius,
                      (retouch_source_anchor_->y() - target_anchor->y())
                          * static_cast<double>(level_zero_height) / creation_radius
                  )};
    } else if (retouch_creation_mode_ != clone_mode) {
        automatic_selection = preview_retouch_source_selection(
            preview_store_,
            preview_generation,
            QSize(level_zero_width, level_zero_height),
            std::span<const QPointF>(
                normalized_points->constData(),
                static_cast<std::size_t>(normalized_points->size())
            ),
            static_cast<double>(creation_radius),
            retouch_creation_mode_
        );
        if (automatic_selection.has_value()) {
            selected_source = automatic_selection->offset_radii;
        }
    }
    const auto fallback_source =
        retouch_creation_mode_ == 1
            ? default_retouch_source_offset(target_anchor->x(), target_anchor->y(), creation_radius)
            : std::pair<double, double>{0.0, 0.0};
    const double source_offset_x =
        selected_source.has_value() ? selected_source->x() : fallback_source.first;
    const double source_offset_y =
        selected_source.has_value() ? selected_source->y() : fallback_source.second;
    if (!retouch_source_offset_fits_detail_apron(
            creation_radius,
            source_offset_x,
            source_offset_y,
            1.0
        )) {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Sampled source is farther than the 512 px detail limit"
        )));
        return;
    }
    grade_stack_.retouch_strokes.push_back(
        BackendRetouchStroke{
            .points = std::move(stroke_points),
            .radius_level_zero_pixels = creation_radius,
            .mode = static_cast<std::uint8_t>(retouch_creation_mode_),
            .source_offset_x_radii = source_offset_x,
            .source_offset_y_radii = source_offset_y,
            .feather = retouch_brush_feather_,
            .strength = retouch_brush_strength_,
            .frequency_radius = static_cast<std::uint16_t>(retouch_sources_->frequencyRadius()),
        }
    );
    if (sampled_source && retouch_source_aligned_
        && !retouch_aligned_source_offset_radii_.has_value()) {
        retouch_aligned_source_offset_radii_ = QPointF(source_offset_x, source_offset_y);
        emit retouchSourceChanged();
    }
    parameterEdited(QStringLiteral("retouch/stroke/add"), before);
    if (!sampled_source && !automatic_selection.has_value()) {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Using a nearby fallback source · drag the outlined source to refine it"
        )));
    } else if (automatic_selection.has_value() && automatic_selection->confidence < 0.38) {
        setStatusMessage(retouch_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Automatic repair source is uncertain · drag the outlined source to refine it"
        )));
    }
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
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.center_x == normalized_x && spot.center_y == normalized_y) {
        return;
    }
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
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    const auto radius = static_cast<std::uint16_t>(radius_level_zero_pixels);
    if (spot.radius_level_zero_pixels == radius) {
        return;
    }
    spot.radius_level_zero_pixels = radius;
    clamp_retouch_source_offset_to_detail_apron(
        radius,
        spot.source_offset_x_radii,
        spot.source_offset_y_radii,
        spot.source_scale
    );
    parameterEdited(QStringLiteral("retouch/%1/radius").arg(index), before);
}

void EditController::setRetouchSpotMode(const int index, const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    constexpr int structure_heal_mode = 2;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || (mode != heal_mode && mode != clone_mode && mode != structure_heal_mode && mode != 3
            && mode != 4)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (spot.mode == encoded_mode) {
        return;
    }
    spot.mode = encoded_mode;
    if (mode == clone_mode && spot.source_offset_x_radii == 0.0
        && spot.source_offset_y_radii == 0.0) {
        const auto [source_offset_x, source_offset_y] = default_retouch_source_offset(
            spot.center_x,
            spot.center_y,
            spot.radius_level_zero_pixels
        );
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
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.feather == feather) {
        return;
    }
    spot.feather = feather;
    parameterEdited(QStringLiteral("retouch/%1/feather").arg(index), before);
}

void EditController::setRetouchSpotStrength(const int index, const double strength) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.strength == strength) {
        return;
    }
    spot.strength = strength;
    parameterEdited(QStringLiteral("retouch/%1/strength").arg(index), before);
}

void EditController::setRetouchSpotSourceOffset(
    const int index,
    const double offset_x_radii,
    const double offset_y_radii
) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(offset_x_radii) || !std::isfinite(offset_y_radii)
        || !retouch_source_offset_fits_detail_apron(
            grade_stack_.retouch_spots[index].radius_level_zero_pixels,
            offset_x_radii,
            offset_y_radii,
            grade_stack_.retouch_spots[index].source_scale
        )) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.source_offset_x_radii == offset_x_radii
        && spot.source_offset_y_radii == offset_y_radii) {
        return;
    }
    spot.source_offset_x_radii = offset_x_radii;
    spot.source_offset_y_radii = offset_y_radii;
    parameterEdited(QStringLiteral("retouch/%1/source").arg(index), before);
}

void EditController::setRetouchSpotSourceTransform(
    const int index,
    const double rotation_degrees,
    const double scale,
    const bool flip_horizontal,
    const bool flip_vertical
) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_spots.size()
        || !std::isfinite(rotation_degrees) || rotation_degrees < -180.0 || rotation_degrees > 180.0
        || !std::isfinite(scale) || scale < 0.25 || scale > 4.0) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& spot = grade_stack_.retouch_spots[index];
    if (spot.source_rotation_degrees == rotation_degrees && spot.source_scale == scale
        && spot.source_flip_horizontal == flip_horizontal
        && spot.source_flip_vertical == flip_vertical) {
        return;
    }
    spot.source_rotation_degrees = rotation_degrees;
    spot.source_scale = scale;
    spot.source_flip_horizontal = flip_horizontal;
    spot.source_flip_vertical = flip_vertical;
    clamp_retouch_source_offset_to_detail_apron(
        spot.radius_level_zero_pixels,
        spot.source_offset_x_radii,
        spot.source_offset_y_radii,
        scale
    );
    parameterEdited(QStringLiteral("retouch/%1/source-transform").arg(index), before);
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
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    const auto radius = static_cast<std::uint16_t>(radius_level_zero_pixels);
    if (stroke.radius_level_zero_pixels == radius) {
        return;
    }
    stroke.radius_level_zero_pixels = radius;
    clamp_retouch_source_offset_to_detail_apron(
        radius,
        stroke.source_offset_x_radii,
        stroke.source_offset_y_radii,
        stroke.source_scale
    );
    parameterEdited(QStringLiteral("retouch/stroke/%1/radius").arg(index), before);
}

void EditController::setRetouchStrokeMode(const int index, const int mode) {
    constexpr int heal_mode = 0;
    constexpr int clone_mode = 1;
    constexpr int structure_heal_mode = 2;
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || (mode != heal_mode && mode != clone_mode && mode != structure_heal_mode && mode != 3
            && mode != 4)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (stroke.mode == encoded_mode) {
        return;
    }
    stroke.mode = encoded_mode;
    if (mode == clone_mode && stroke.source_offset_x_radii == 0.0
        && stroke.source_offset_y_radii == 0.0) {
        const BackendRetouchStrokePoint source_center =
            stroke.points.isEmpty() ? BackendRetouchStrokePoint{.x = 0.5, .y = 0.5}
                                    : stroke.points.front();
        const auto [source_offset_x, source_offset_y] = default_retouch_source_offset(
            source_center.x,
            source_center.y,
            stroke.radius_level_zero_pixels
        );
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
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.feather == feather) {
        return;
    }
    stroke.feather = feather;
    parameterEdited(QStringLiteral("retouch/stroke/%1/feather").arg(index), before);
}

void EditController::setRetouchStrokeStrength(const int index, const double strength) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.strength == strength) {
        return;
    }
    stroke.strength = strength;
    parameterEdited(QStringLiteral("retouch/stroke/%1/strength").arg(index), before);
}

void EditController::setRetouchStrokeSourceOffset(
    const int index,
    const double offset_x_radii,
    const double offset_y_radii
) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(offset_x_radii) || !std::isfinite(offset_y_radii)
        || !retouch_source_offset_fits_detail_apron(
            grade_stack_.retouch_strokes[index].radius_level_zero_pixels,
            offset_x_radii,
            offset_y_radii,
            grade_stack_.retouch_strokes[index].source_scale
        )) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.source_offset_x_radii == offset_x_radii
        && stroke.source_offset_y_radii == offset_y_radii) {
        return;
    }
    stroke.source_offset_x_radii = offset_x_radii;
    stroke.source_offset_y_radii = offset_y_radii;
    parameterEdited(QStringLiteral("retouch/stroke/%1/source").arg(index), before);
}

void EditController::setRetouchStrokeSourceTransform(
    const int index,
    const double rotation_degrees,
    const double scale,
    const bool flip_horizontal,
    const bool flip_vertical
) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(rotation_degrees) || rotation_degrees < -180.0 || rotation_degrees > 180.0
        || !std::isfinite(scale) || scale < 0.25 || scale > 4.0) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.source_rotation_degrees == rotation_degrees && stroke.source_scale == scale
        && stroke.source_flip_horizontal == flip_horizontal
        && stroke.source_flip_vertical == flip_vertical) {
        return;
    }
    stroke.source_rotation_degrees = rotation_degrees;
    stroke.source_scale = scale;
    stroke.source_flip_horizontal = flip_horizontal;
    stroke.source_flip_vertical = flip_vertical;
    clamp_retouch_source_offset_to_detail_apron(
        stroke.radius_level_zero_pixels,
        stroke.source_offset_x_radii,
        stroke.source_offset_y_radii,
        scale
    );
    parameterEdited(QStringLiteral("retouch/stroke/%1/source-transform").arg(index), before);
}

void EditController::translateRetouchStroke(
    const int index,
    const double normalized_dx,
    const double normalized_dy
) {
    if (!active_ || interactionLocked() || index < 0 || index >= grade_stack_.retouch_strokes.size()
        || !std::isfinite(normalized_dx) || !std::isfinite(normalized_dy)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    auto& stroke = grade_stack_.retouch_strokes[index];
    if (stroke.points.isEmpty()) {
        return;
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
    const double clamped_dx = std::clamp(normalized_dx, -lower_x, 1.0 - upper_x);
    const double clamped_dy = std::clamp(normalized_dy, -lower_y, 1.0 - upper_y);
    if (clamped_dx == 0.0 && clamped_dy == 0.0) {
        return;
    }
    for (auto& point : stroke.points) {
        point.x += clamped_dx;
        point.y += clamped_dy;
    }
    parameterEdited(QStringLiteral("retouch/stroke/%1/position").arg(index), before);
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
