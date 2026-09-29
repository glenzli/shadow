#include "edit_controller.hpp"
#include "edit_targeted_curve_controller.hpp"

#include <initializer_list>

namespace {

constexpr int TONE_CURVE_PREVIEW_THROTTLE_MS = 16;

[[nodiscard]] LocalizedUiMessage tone_curve_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] bool is_neutral_oklab_lightness_curve(const QVector<double>& points) noexcept {
    return points.isEmpty()
           || (points.size() == 4 && points[0] == 0.0 && points[1] == 0.0 && points[2] == 1.0
               && points[3] == 1.0);
}

template <typename Fine> auto& selected_curve_values(Fine& fine, const int channel) {
    return channel == 0 ? fine.oklab_lightness_curve_points
                        : fine.rgb_curve_points[static_cast<std::size_t>(channel - 1)];
}

[[nodiscard]] QVector<double>
backend_oklab_lightness_curve_points(const ToneCurvePointModel& model) {
    const auto source = model.points();
    QVector<double> points;
    points.reserve(source.size() * 2);
    for (const auto& point : source) {
        points.push_back(point.x);
        points.push_back(point.y);
    }
    return points;
}

[[nodiscard]] QString tone_curve_gesture_key(const int channel, const int index) {
    return QStringLiteral("tone_curve/%1/point/%2").arg(channel).arg(index);
}

} // namespace

QVector<ToneCurvePoint> EditController::toneCurveModelPoints(const BackendGradeNode* node) const {
    if (node == nullptr)
        return {{0.0, 0.0}, {1.0, 1.0}};
    const auto& source = selected_curve_values(node->fine, tone_curve_channel_);
    if (source.isEmpty())
        return {{0.0, 0.0}, {1.0, 1.0}};
    QVector<ToneCurvePoint> result;
    for (qsizetype i = 0; i + 1 < source.size(); i += 2)
        result.push_back({source[i], source[i + 1]});
    return result;
}

void EditController::setToneCurveChannel(const int channel) {
    if (channel < 0 || channel > 4 || channel == tone_curve_channel_)
        return;
    targeted_curve_controller_->finish(false);
    const int point = tone_curve_points_.selectedIndex();
    const auto gesture = tone_curve_gesture_key(tone_curve_channel_, point);
    if (active_parameter_gestures_.contains(gesture))
        endParameterEdit(gesture);
    tone_curve_channel_ = channel;
    static_cast<void>(tone_curve_points_.replace(toneCurveModelPoints(selectedGradeNode())));
    emit toneCurveChanged();
}

QVariantList EditController::toneCurveModifiedChannels() const {
    QVariantList result;
    const auto* node = selectedGradeNode();
    for (int channel = 0; channel < 5; ++channel)
        result.push_back(
            node && !is_neutral_oklab_lightness_curve(selected_curve_values(node->fine, channel))
        );
    return result;
}

void EditController::resetAllToneCurves() {
    const auto* node = selectedGradeNode();
    if (!active_ || interactionLocked() || !node || !node->enabled)
        return;
    const auto before = grade_stack_;
    auto& fine = grade_stack_.grade_nodes[selected_grade_node_index_].fine;
    fine.oklab_lightness_curve_points.clear();
    for (auto& curve : fine.rgb_curve_points)
        curve.clear();
    if (before == grade_stack_)
        return;
    tone_curve_points_.resetLinear();
    toneCurveEdited(QStringLiteral("tone_curve/reset_all"), before, 0);
}

QAbstractItemModel* EditController::toneCurvePoints() noexcept {
    return &tone_curve_points_;
}

bool EditController::hasToneCurve() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr
           && !is_neutral_oklab_lightness_curve(
               selected_curve_values(grade_node->fine, tone_curve_channel_)
           );
}

bool EditController::toneCurveEditable() const noexcept {
    return tone_curve_points_.isEditable();
}

void EditController::beginToneCurveGesture(const int index) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || !tone_curve_points_.isEditable() || !tone_curve_points_.selectPoint(index)) {
        return;
    }
    beginParameterEdit(tone_curve_gesture_key(tone_curve_channel_, index));
}

void EditController::moveToneCurvePoint(const int index, const double x, const double y) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (!tone_curve_points_.movePoint(index, x, y)) {
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    selected_curve_values(edited.fine, tone_curve_channel_) =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    toneCurveEdited(
        tone_curve_gesture_key(tone_curve_channel_, index),
        before,
        TONE_CURVE_PREVIEW_THROTTLE_MS
    );
}

void EditController::endToneCurveGesture(const int index) {
    endParameterEdit(tone_curve_gesture_key(tone_curve_channel_, index));
}

void EditController::addToneCurvePoint(const double x, const double y) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    if (tone_curve_points_.addPoint(x, y) < 0) {
        setStatusMessage(tone_curve_message(
            QT_TRANSLATE_NOOP("EditController", "The point cannot be added inside this curve")
        ));
        return;
    }
    auto& edited = grade_stack_.grade_nodes[selected_grade_node_index_];
    selected_curve_values(edited.fine, tone_curve_channel_) =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    toneCurveEdited(QStringLiteral("tone_curve/%1/add").arg(tone_curve_channel_), before, 0);
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
    selected_curve_values(edited.fine, tone_curve_channel_) =
        backend_oklab_lightness_curve_points(tone_curve_points_);
    auto& values = selected_curve_values(edited.fine, tone_curve_channel_);
    if (is_neutral_oklab_lightness_curve(values))
        values.clear();
    toneCurveEdited(QStringLiteral("tone_curve/%1/remove").arg(tone_curve_channel_), before, 0);
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
    selected_curve_values(edited.fine, tone_curve_channel_).clear();
    toneCurveEdited(QStringLiteral("tone_curve/%1/reset").arg(tone_curve_channel_), before, 0);
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
