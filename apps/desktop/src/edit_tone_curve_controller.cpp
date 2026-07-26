#include "edit_controller.hpp"

#include <initializer_list>

namespace {

constexpr int TONE_CURVE_PREVIEW_THROTTLE_MS = 16;

[[nodiscard]] LocalizedUiMessage tone_curve_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
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

} // namespace

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
        TONE_CURVE_PREVIEW_THROTTLE_MS
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
        setStatusMessage(tone_curve_message(QT_TRANSLATE_NOOP(
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
