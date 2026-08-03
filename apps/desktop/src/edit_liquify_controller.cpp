#include "edit_controller.hpp"
#include "edit_liquify_coordinates.hpp"
#include "edit_stroke_input.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>

namespace {

constexpr qsizetype MAXIMUM_LIQUIFY_STROKES = 128;
constexpr qsizetype MAXIMUM_LIQUIFY_POINTS_PER_STROKE = 2'048;
constexpr int LIQUIFY_PUSH_MODE = 0;
constexpr int LIQUIFY_RECONSTRUCT_MODE = 1;

[[nodiscard]] LocalizedUiMessage liquify_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] std::optional<QVector<BackendLiquifyPoint>> original_liquify_points(
    const QVariantList& points,
    const double output_aspect_ratio,
    const BackendPhotoGeometry& geometry,
    const qsizetype minimum_point_count
) {
    const auto output_points =
        EditStrokeInput::decodeNormalizedSamples(points, MAXIMUM_LIQUIFY_POINTS_PER_STROKE);
    if (!output_points.has_value() || output_points->size() < minimum_point_count) {
        return std::nullopt;
    }

    QVector<BackendLiquifyPoint> original_points;
    original_points.reserve(output_points->size());
    for (const auto& point : *output_points) {
        const auto original = EditLiquifyCoordinates::originalPointForOutput(
            point.position,
            output_aspect_ratio,
            geometry
        );
        if (!original.has_value()) {
            return std::nullopt;
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
    if (original_points.size() < minimum_point_count) {
        return std::nullopt;
    }
    return original_points;
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
                {QStringLiteral("kind"), static_cast<int>(stroke.kind)},
                {QStringLiteral("points"), points},
                {QStringLiteral("radius"), stroke.radius},
                {QStringLiteral("strength"), stroke.strength},
                {QStringLiteral("hardness"), stroke.hardness},
            }
        );
    }
    return result;
}

bool EditController::liquifyNodeEnabled() const noexcept {
    return !grade_stack_.liquify_strokes.isEmpty() && grade_stack_.liquify_enabled;
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

int EditController::liquifyBrushMode() const noexcept {
    return liquify_brush_mode_;
}

bool EditController::liquifyCanReconstruct() const noexcept {
    return std::any_of(
        grade_stack_.liquify_strokes.cbegin(),
        grade_stack_.liquify_strokes.cend(),
        [](const BackendLiquifyStroke& stroke) { return stroke.kind == LIQUIFY_PUSH_MODE; }
    );
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

void EditController::setLiquifyBrushMode(const int mode) {
    if ((mode != LIQUIFY_PUSH_MODE && mode != LIQUIFY_RECONSTRUCT_MODE)
        || liquify_brush_mode_ == mode) {
        return;
    }
    if (liquify_live_before_.has_value()) {
        cancelLiquifyLiveStroke();
    }
    liquify_brush_mode_ = mode;
    emit liquifyBrushChanged();
}

void EditController::setLiquifyNodeEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.liquify_strokes.isEmpty()
        || grade_stack_.liquify_enabled == enabled) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.liquify_enabled = enabled;
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("liquify/enabled"), before);
    setStatusMessage(liquify_message(
        enabled ? QT_TRANSLATE_NOOP("EditController", "Liquify enabled")
                : QT_TRANSLATE_NOOP("EditController", "Liquify bypassed · strokes preserved")
    ));
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
    auto original_points =
        original_liquify_points(points, output_aspect_ratio, grade_stack_.geometry, 2);
    if (!original_points.has_value()) {
        return;
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.liquify_enabled = true;
    grade_stack_.liquify_strokes.push_back(
        BackendLiquifyStroke{
            .kind = LIQUIFY_PUSH_MODE,
            .points = std::move(*original_points),
            .radius = liquify_brush_radius_,
            .strength = liquify_brush_strength_,
            .hardness = liquify_brush_hardness_,
        }
    );
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("liquify/stroke/add"), before);
    setStatusMessage(liquify_message(QT_TRANSLATE_NOOP("EditController", "Added Liquify stroke")));
}

bool EditController::beginLiquifyLiveStroke() {
    const int kind = liquify_brush_mode_;
    const bool reconstruct = kind == LIQUIFY_RECONSTRUCT_MODE;
    if (!active_ || interactionLocked()
        || (kind != LIQUIFY_PUSH_MODE && kind != LIQUIFY_RECONSTRUCT_MODE)
        || (reconstruct && (!grade_stack_.liquify_enabled || !liquifyCanReconstruct()))
        || grade_stack_.liquify_strokes.size() >= MAXIMUM_LIQUIFY_STROKES
        || liquify_live_before_.has_value()) {
        return false;
    }
    constexpr auto gesture_key = "liquify/stroke/live";
    beginParameterEdit(QString::fromLatin1(gesture_key));
    if (!active_parameter_gestures_.contains(QString::fromLatin1(gesture_key))) {
        return false;
    }
    liquify_live_before_ = grade_stack_;
    liquify_live_index_ = -1;
    liquify_live_kind_ = kind;
    // A live stroke is preview-only until release. Preserve any earlier
    // pending autosave, but do not let it capture an in-flight path.
    autosave_debounce_.stop();
    return true;
}

void EditController::updateLiquifyLiveStrokeFromPreview(
    const QVariantList& points,
    const double output_aspect_ratio
) {
    if (!liquify_live_before_.has_value() || !active_
        || liquify_brush_mode_ != liquify_live_kind_) {
        return;
    }
    const qsizetype minimum_point_count =
        liquify_live_kind_ == LIQUIFY_RECONSTRUCT_MODE ? 1 : 2;
    auto original_points =
        original_liquify_points(
            points,
            output_aspect_ratio,
            grade_stack_.geometry,
            minimum_point_count
        );
    if (!original_points.has_value()) {
        return;
    }

    const BackendLiquifyStroke stroke{
        .kind = static_cast<std::uint8_t>(liquify_live_kind_),
        .points = std::move(*original_points),
        .radius = liquify_brush_radius_,
        .strength = liquify_brush_strength_,
        .hardness = liquify_brush_hardness_,
    };
    if (liquify_live_index_ < 0) {
        liquify_live_index_ = grade_stack_.liquify_strokes.size();
        grade_stack_.liquify_strokes.push_back(stroke);
    } else if (liquify_live_index_ < grade_stack_.liquify_strokes.size()) {
        grade_stack_.liquify_strokes[liquify_live_index_] = stroke;
    } else {
        cancelLiquifyLiveStroke();
        return;
    }
    grade_stack_.liquify_enabled = true;
    setFullResolutionState(false, false, 0);
    notifyParametersChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(16);
}

void EditController::finishLiquifyLiveStroke() {
    if (!liquify_live_before_.has_value()) {
        return;
    }
    const BackendGradeStack before = *liquify_live_before_;
    const int kind = liquify_live_kind_;
    const bool changed = liquify_live_index_ >= 0 && grade_stack_ != before;
    liquify_live_before_.reset();
    liquify_live_index_ = -1;
    liquify_live_kind_ = -1;
    endParameterEdit(QStringLiteral("liquify/stroke/live"));
    if (!changed) {
        if (autosave_requested_) {
            scheduleAutosave();
        }
        return;
    }
    ++working_revision_;
    autosave_requested_ = true;
    clearAutosaveFailure();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    if (dirty_ && !stateTaskRunning()) {
        scheduleAutosave();
    }
    setStatusMessage(liquify_message(
        kind == LIQUIFY_RECONSTRUCT_MODE
            ? QT_TRANSLATE_NOOP("EditController", "Added Liquify reconstruction")
            : QT_TRANSLATE_NOOP("EditController", "Added Liquify stroke")
    ));
}

void EditController::cancelLiquifyLiveStroke() {
    if (!liquify_live_before_.has_value()) {
        return;
    }
    BackendGradeStack before = std::move(*liquify_live_before_);
    liquify_live_before_.reset();
    liquify_live_index_ = -1;
    liquify_live_kind_ = -1;
    setGradeStack(std::move(before));
    endParameterEdit(QStringLiteral("liquify/stroke/live"));
    if (autosave_requested_) {
        scheduleAutosave();
    }
}

void EditController::clearLiquify() {
    if (!active_ || interactionLocked() || grade_stack_.liquify_strokes.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.liquify_enabled = false;
    grade_stack_.liquify_strokes.clear();
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("liquify/clear"), before);
    setStatusMessage(liquify_message(QT_TRANSLATE_NOOP("EditController", "Removed Liquify")));
}
