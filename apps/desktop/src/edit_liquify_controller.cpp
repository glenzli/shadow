#include "edit_controller.hpp"
#include "edit_liquify_coordinates.hpp"
#include "edit_stroke_input.hpp"

#include <QVariantMap>

#include <cmath>
#include <initializer_list>
#include <utility>

namespace {

constexpr qsizetype MAXIMUM_LIQUIFY_STROKES = 128;
constexpr qsizetype MAXIMUM_LIQUIFY_POINTS_PER_STROKE = 2'048;

[[nodiscard]] LocalizedUiMessage liquify_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

QVariantList EditController::liquifyStrokes() const {
    QVariantList result;
    result.reserve(grade_stack_.liquify_strokes.size());
    for (qsizetype index = 0; index < grade_stack_.liquify_strokes.size(); ++index) {
        const auto& stroke = grade_stack_.liquify_strokes.at(index);
        QVariantList points;
        points.reserve(stroke.points.size());
        for (const auto& point : stroke.points) {
            points.push_back(
                QVariantMap{
                    {QStringLiteral("x"), point.x},
                    {QStringLiteral("y"), point.y},
                    {QStringLiteral("pressure"), point.pressure},
                }
            );
        }
        result.push_back(
            QVariantMap{
                {QStringLiteral("index"), static_cast<int>(index)},
                {QStringLiteral("points"), points},
                {QStringLiteral("radius"), stroke.radius},
                {QStringLiteral("strength"), stroke.strength},
                {QStringLiteral("hardness"), stroke.hardness},
            }
        );
    }
    return result;
}

double EditController::liquifyBrushRadius() const noexcept {
    return liquify_brush_radius_;
}

double EditController::liquifyBrushStrength() const noexcept {
    return liquify_brush_strength_;
}

double EditController::liquifyBrushHardness() const noexcept {
    return liquify_brush_hardness_;
}

void EditController::setLiquifyBrushRadius(const double radius) {
    constexpr double minimum_radius = 0.005;
    constexpr double maximum_radius = 0.25;
    if (!std::isfinite(radius) || radius < minimum_radius || radius > maximum_radius
        || liquify_brush_radius_ == radius) {
        return;
    }
    liquify_brush_radius_ = radius;
    emit liquifyBrushChanged();
}

void EditController::setLiquifyBrushStrength(const double strength) {
    constexpr double minimum_strength = 0.01;
    if (!std::isfinite(strength) || strength < minimum_strength || strength > 1.0
        || liquify_brush_strength_ == strength) {
        return;
    }
    liquify_brush_strength_ = strength;
    emit liquifyBrushChanged();
}

void EditController::setLiquifyBrushHardness(const double hardness) {
    if (!std::isfinite(hardness) || hardness < 0.0 || hardness > 1.0
        || liquify_brush_hardness_ == hardness) {
        return;
    }
    liquify_brush_hardness_ = hardness;
    emit liquifyBrushChanged();
}

void EditController::addLiquifyStrokeFromPreview(
    const QVariantList& points,
    const double output_aspect_ratio
) {
    if (!active_ || interactionLocked()) {
        return;
    }
    if (grade_stack_.liquify_strokes.size() >= MAXIMUM_LIQUIFY_STROKES) {
        setStatusMessage(liquify_message(
            QT_TRANSLATE_NOOP("EditController", "Liquify supports at most 128 strokes")
        ));
        return;
    }
    const auto output_points =
        EditStrokeInput::decodeNormalizedSamples(points, MAXIMUM_LIQUIFY_POINTS_PER_STROKE);
    if (!output_points.has_value() || output_points->size() < 2) {
        return;
    }

    QVector<BackendLiquifyPoint> original_points;
    original_points.reserve(output_points->size());
    for (const auto& point : *output_points) {
        const auto original = EditLiquifyCoordinates::originalPointForOutput(
            point.position,
            output_aspect_ratio,
            grade_stack_.geometry
        );
        if (!original.has_value()) {
            return;
        }
        const BackendLiquifyPoint decoded{
            .x = original->x(),
            .y = original->y(),
            .pressure = point.pressure,
        };
        if (original_points.isEmpty() || original_points.constLast() != decoded) {
            original_points.push_back(decoded);
        }
    }
    if (original_points.size() < 2) {
        return;
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.liquify_strokes.push_back(
        BackendLiquifyPushStroke{
            .points = std::move(original_points),
            .radius = liquify_brush_radius_,
            .strength = liquify_brush_strength_,
            .hardness = liquify_brush_hardness_,
        }
    );
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("liquify/stroke/add"), before);
    setStatusMessage(liquify_message(QT_TRANSLATE_NOOP("EditController", "Added Liquify stroke")));
}

void EditController::clearLiquify() {
    if (!active_ || interactionLocked() || grade_stack_.liquify_strokes.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.liquify_strokes.clear();
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("liquify/clear"), before);
    setStatusMessage(liquify_message(QT_TRANSLATE_NOOP("EditController", "Removed Liquify")));
}
