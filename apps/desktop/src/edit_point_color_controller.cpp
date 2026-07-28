#include "edit_controller.hpp"

#include "edit_point_color_model.hpp"

#include <QColor>
#include <QImage>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace {

constexpr int MAX_POINT_COLOR_COUNT = 16;

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

[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
    return {"EditController", source, arguments};
}

} // namespace

QVariantList EditController::pointColors() const {
    QVariantList result;
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return result;
    }
    const int count = PointColorModel::count(grade_node->fine);
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        const auto range = PointColorModel::at(grade_node->fine, index);
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

std::optional<PreviewScopeHueQualifier> EditController::selectedPointColorScopeQualifier() const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || selected_point_color_index_ < 0
        || selected_point_color_index_ >= PointColorModel::count(grade_node->fine)) {
        return std::nullopt;
    }
    const BackendPointColorRange range = PointColorModel::at(
        grade_node->fine,
        selected_point_color_index_
    );
    if (!range.enabled || !std::isfinite(range.center_degrees)
        || range.center_degrees < 0.0 || range.center_degrees > 360.0
        || !std::isfinite(range.width_degrees) || range.width_degrees < 1.0
        || range.width_degrees > 180.0 || !std::isfinite(range.softness)
        || range.softness < 0.0 || range.softness > 1.0) {
        return std::nullopt;
    }
    return PreviewScopeHueQualifier{
        .center_degrees = range.center_degrees,
        .width_degrees = range.width_degrees,
        .softness = range.softness,
    };
}

bool EditController::pointColorScopeActive() const noexcept {
    return point_color_scope_active_;
}

bool EditController::pointColorScopeAvailable() const noexcept {
    return selectedPointColorScopeQualifier().has_value();
}

bool EditController::pointColorPickerActive() const noexcept {
    return point_color_picker_active_;
}

bool EditController::whiteBalancePickerActive() const noexcept {
    return white_balance_picker_active_;
}

void EditController::selectPointColor(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr || index < 0 || index >= PointColorModel::count(grade_node->fine)
        || index == selected_point_color_index_) {
        return;
    }
    finishActiveGesture();
    selected_point_color_index_ = index;
    clearPointColorScopeReference();
    notifyParametersChanged();
}

void EditController::setPointColorScopeActive(const bool active) {
    const bool next = active && pointColorScopeAvailable();
    if (point_color_scope_active_ == next) {
        return;
    }
    point_color_scope_active_ = next;
    clearPointColorScopeReference();
    refreshCurrentDisplayScope();
    emit pointColorScopeChanged();
}

void EditController::clearPointColorScopeReference() noexcept {
    point_color_scope_reference_.reset();
}

void EditController::removeSelectedPointColor() {
    if (!active_ || interactionLocked()) {
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || selected_point_color_index_ < 0
        || selected_point_color_index_ >= PointColorModel::count(grade_node->fine)) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    auto& fine = grade_node->fine;
    if (selected_point_color_index_ == 0) {
        if (!fine.additional_point_colors.isEmpty()) {
            const BackendPointColorRange promoted = fine.additional_point_colors.takeFirst();
            PointColorModel::set(fine, 0, promoted);
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
    const int count = PointColorModel::count(fine);
    selected_point_color_index_ = count == 0
        ? -1 : std::min(selected_point_color_index_, count - 1);
    clearPointColorScopeReference();
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
    if (active && retouch_picker_active_) {
        endRetouchStroke();
        retouch_picker_active_ = false;
        emit retouchPickerActiveChanged();
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
    if (active && retouch_picker_active_) {
        endRetouchStroke();
        retouch_picker_active_ = false;
        emit retouchPickerActiveChanged();
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
    if (PointColorModel::count(fine) >= MAX_POINT_COLOR_COUNT) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Point Color supports at most 16 samples"
        )));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    if (PointColorModel::count(fine) == 0) {
        PointColorModel::set(fine, 0, range);
        selected_point_color_index_ = 0;
    } else {
        fine.additional_point_colors.push_back(range);
        selected_point_color_index_ = PointColorModel::count(fine) - 1;
    }
    clearPointColorScopeReference();
    parameterEdited(QStringLiteral("point_color/add"), before);
    setPointColorPickerActive(false);
}
