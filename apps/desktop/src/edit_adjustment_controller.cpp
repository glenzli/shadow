#include "edit_controller.hpp"

#include "edit_fine_parameter_registry.hpp"
#include "edit_point_color_model.hpp"

#include <QFileInfo>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

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

QVariantList EditController::colorWarperControlPoints() const {
    QVariantList result;
    const auto* const grade_node = selectedGradeNode();
    const BackendFineEditParameters neutral;
    const auto& points = grade_node == nullptr
        ? neutral.oklab_color_warper_control_points
        : grade_node->fine.oklab_color_warper_control_points;
    result.reserve(static_cast<qsizetype>(points.size()));
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto& point = points[index];
        result.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("row"), static_cast<int>(
                index / BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE)},
            {QStringLiteral("column"), static_cast<int>(
                index % BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE)},
            {QStringLiteral("aOffset"), point.a_offset},
            {QStringLiteral("bOffset"), point.b_offset},
        });
    }
    return result;
}

double EditController::parameterValue(const QString& parameter_key) const {
    const auto* const grade_node = selectedGradeNode();
    const BackendFineEditParameters neutral;
    const BackendFineEditParameters& fine = grade_node == nullptr
        ? neutral
        : grade_node->fine;
    if (const auto* const descriptor =
            EditFineParameterRegistry::find(QStringView{parameter_key});
        descriptor != nullptr) {
        return fine.*(descriptor->member);
    }
    if (parameter_key.startsWith(QStringLiteral("color_range_"))) {
        if (selected_point_color_index_ < 0
            || selected_point_color_index_ >= PointColorModel::count(fine)) {
            return parameter_key == QStringLiteral("color_range_width") ? 30.0
                : parameter_key == QStringLiteral("color_range_softness") ? 0.5 : 0.0;
        }
        const auto range = PointColorModel::at(fine, selected_point_color_index_);
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
        const int count = PointColorModel::count(fine);
        if (selected_point_color_index_ < 0 || selected_point_color_index_ >= count) {
            return;
        }
        BackendPointColorRange range = PointColorModel::at(fine, selected_point_color_index_);
        double minimum = -1.0;
        double maximum = 1.0;
        double current = 0.0;
        if (parameter_key == QStringLiteral("color_range_enabled")) {
            minimum = 0.0;
            maximum = 1.0;
            current = range.enabled ? 1.0 : 0.0;
        } else if (parameter_key == QStringLiteral("color_range_center")) {
            minimum = 0.0;
            maximum = 360.0;
            current = range.center_degrees;
        } else if (parameter_key == QStringLiteral("color_range_width")) {
            minimum = 1.0;
            maximum = 180.0;
            current = range.width_degrees;
        } else if (parameter_key == QStringLiteral("color_range_softness")) {
            minimum = 0.0;
            maximum = 1.0;
            current = range.softness;
        } else if (parameter_key == QStringLiteral("color_range_hue")) {
            minimum = -180.0;
            maximum = 180.0;
            current = range.hue_shift_degrees;
        } else if (parameter_key == QStringLiteral("color_range_saturation")) {
            current = range.saturation;
        } else if (parameter_key == QStringLiteral("color_range_lightness")) {
            current = range.lightness;
        } else {
            return;
        }
        if (current == value
            || !acceptParameter(
                value,
                minimum,
                maximum,
                QT_TRANSLATE_NOOP("EditController", "Point Color")
            )) {
            return;
        }
        const BackendGradeStack before = grade_stack_;
        if (parameter_key == QStringLiteral("color_range_enabled")) {
            range.enabled = value >= 0.5;
        } else if (parameter_key == QStringLiteral("color_range_center")) {
            range.center_degrees = value;
        } else if (parameter_key == QStringLiteral("color_range_width")) {
            range.width_degrees = value;
        } else if (parameter_key == QStringLiteral("color_range_softness")) {
            range.softness = value;
        } else if (parameter_key == QStringLiteral("color_range_hue")) {
            range.hue_shift_degrees = value;
        } else if (parameter_key == QStringLiteral("color_range_saturation")) {
            range.saturation = value;
        } else if (parameter_key == QStringLiteral("color_range_lightness")) {
            range.lightness = value;
        }
        PointColorModel::set(fine, selected_point_color_index_, range);
        parameterEdited(
            QStringLiteral("point_color/%1/%2")
                .arg(selected_point_color_index_)
                .arg(parameter_key),
            before
        );
        return;
    }
    const auto* const descriptor =
        EditFineParameterRegistry::find(QStringView{parameter_key});
    if (descriptor == nullptr || !descriptor->writable()) {
        return;
    }
    double& target = fine.*(descriptor->member);
    if (target == value
        || !acceptParameter(
            value,
            descriptor->minimum,
            descriptor->maximum,
            descriptor->label_source
        )) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    target = value;
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

void EditController::setColorWarperControlPoint(
    const int index,
    const double a_offset,
    const double b_offset
) {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr || !selected->enabled
        || index < 0
        || static_cast<std::size_t>(index) >= BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT
        || !std::isfinite(a_offset)
        || !std::isfinite(b_offset)
        || a_offset < -BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || a_offset > BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || b_offset < -BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || b_offset > BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET
        || !acceptParameter(
            a_offset,
            -BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            QT_TRANSLATE_NOOP("EditController", "Color Warper control point")
        )) {
        return;
    }
    auto& point = grade_stack_.grade_nodes[selected_grade_node_index_]
                      .fine
                      .oklab_color_warper_control_points[static_cast<std::size_t>(index)];
    if (point.a_offset == a_offset && point.b_offset == b_offset) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    point = {.a_offset = a_offset, .b_offset = b_offset};
    parameterEdited(QStringLiteral("color_warper/point/%1").arg(index), before);
}

void EditController::resetColorWarper() {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr || !selected->enabled) {
        return;
    }
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    const auto neutral_points = decltype(fine.oklab_color_warper_control_points){};
    if (fine.oklab_color_warper_control_points == neutral_points
        && fine.oklab_color_warper_strength == 1.0) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    fine.oklab_color_warper_control_points = neutral_points;
    fine.oklab_color_warper_strength = 1.0;
    parameterEdited(QStringLiteral("color_warper/reset"), before);
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
    auto* const grade_node = selected_grade_node_index_ < 0
        ? nullptr : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->fine.selective_color_relative == relative) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].fine.selective_color_relative = relative;
    parameterEdited(QStringLiteral("selective_color/method"), before);
}
