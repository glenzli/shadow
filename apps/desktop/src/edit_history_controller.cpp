#include "edit_controller.hpp"

#include "edit_history_restore_projection.hpp"
#include "edit_point_color_model.hpp"
#include "edit_stack.hpp"

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <utility>

namespace {

constexpr int EDIT_PREVIEW_THROTTLE_MS = 16;

[[nodiscard]] LocalizedUiMessage edit_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] QVector<ToneCurvePoint>
tone_curve_model_points(const BackendGradeNode* const grade_node) {
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

} // namespace

void EditController::beginParameterEdit(const QString& parameter_key) {
    const auto* const grade_node = selectedGradeNode();
    const bool photo_local_retouch = parameter_key.startsWith(QStringLiteral("retouch/"));
    const bool photo_local_geometry = parameter_key.startsWith(QStringLiteral("geometry/"));
    if (!active_ || interactionLocked()
        || (!photo_local_retouch && !photo_local_geometry
            && (grade_node == nullptr || !grade_node->enabled))
        || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    if (active_parameter_gestures_.isEmpty()) {
        first_interactive_frame_presented_ = false;
    }
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
        first_interactive_frame_presented_ = false;
        cancelActivePreview(true);
        schedulePreview(0);
    }
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
    const QString preferred_id = EditHistoryRestoreProjection::preferredGradeNodeForRestore(
        history_key,
        *restored,
        selected_grade_node_index_
    );
    autosave_requested_ = true;
    clearAutosaveFailure();
    ++working_revision_;
    setGradeStack(*restored, preferred_id);
    schedulePreview(0);
    setStatusMessage(
        edit_message(QT_TRANSLATE_NOOP("EditController", "Undid the last session adjustment"))
    );
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
    setGradeStack(
        *restored,
        EditHistoryRestoreProjection::preferredGradeNodeForRestore(
            history_key,
            *restored,
            selected_grade_node_index_
        )
    );
    schedulePreview(0);
    setStatusMessage(
        edit_message(QT_TRANSLATE_NOOP("EditController", "Redid the last session adjustment"))
    );
}

void EditController::resetAllAdjustments() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();

    BackendGradeNode neutral;
    try {
        neutral = backend_->newBasicGradeNode(QStringLiteral("Adjustments"));
    } catch (const std::exception& error) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController", "Could not reset adjustments · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset;
    reset.grade_nodes = {neutral};
    setFullResolutionState(false, false, 0);
    setGradeStack(std::move(reset), neutral.grade_node_id);
    recordWorkingTransition(QStringLiteral("adjustments/reset_all"), before);
    schedulePreview(0);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP("EditController", "Reset all adjustments")));
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
            "EditController",
            "Discarded working changes and restored the Library version"
        )));
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
    setStatusMessage(
        edit_message(QT_TRANSLATE_NOOP("EditController", "Restored the current saved version"))
    );
}

void EditController::setGradeStack(
    BackendGradeStack grade_stack,
    const QString& preferred_grade_node_id
) {
    if (grade_stack.grade_nodes.size() > GradeNodeStack::maximum_grade_node_count) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The saved edit exceeds the 16-Grade-Node desktop limit"
        )));
        return;
    }
    const auto old_optics = grade_stack_.optics;
    const QString old_selected_id = selectedGradeNodeId();
    const int old_selected_index = selected_grade_node_index_;
    const BackendGradeNode* const old_selected = selectedGradeNode();
    const bool had_old_selection = old_selected != nullptr;
    const BackendGradeNode old_selected_value =
        had_old_selection ? *old_selected : BackendGradeNode{};
    const QString requested_id =
        preferred_grade_node_id.isEmpty() ? old_selected_id : preferred_grade_node_id;
    const int new_selected_index =
        GradeNodeStack::resolvedSelection(grade_stack, requested_id, old_selected_index);
    const BackendGradeNode* const new_selected =
        new_selected_index < 0 ? nullptr : &grade_stack.grade_nodes.at(new_selected_index);
    const bool has_new_selection = new_selected != nullptr;
    const bool selection_changed =
        old_selected_index != new_selected_index
        || old_selected_id != (has_new_selection ? new_selected->grade_node_id : QString{});
    const bool grade_node_enabled_changed =
        selection_changed || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && old_selected_value.enabled != new_selected->enabled);
    const bool basic_changed = selection_changed || had_old_selection != has_new_selection
                               || (had_old_selection && has_new_selection
                                   && (old_selected_value.basic != new_selected->basic
                                       || old_selected_value.fine != new_selected->fine));
    const bool local_mask_changed =
        selection_changed || had_old_selection != has_new_selection
        || (had_old_selection && has_new_selection
            && EditHistoryRestoreProjection::localMaskChanged(old_selected_value, *new_selected));
    const bool retouch_changed = grade_stack_.retouch_spots != grade_stack.retouch_spots
                                 || grade_stack_.retouch_strokes != grade_stack.retouch_strokes;
    const bool curve_changed = selection_changed || had_old_selection != has_new_selection
                               || (had_old_selection && has_new_selection
                                   && old_selected_value.fine.oklab_lightness_curve_points
                                          != new_selected->fine.oklab_lightness_curve_points);
    const bool list_changed =
        EditHistoryRestoreProjection::gradeNodeListChanged(grade_stack_, grade_stack);
    const auto model_points = tone_curve_model_points(new_selected);
    if (tone_curve_points_.points() != model_points && !tone_curve_points_.replace(model_points)) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController", "The saved Tone Curve cannot be represented safely")
        ));
        return;
    }
    grade_stack_ = std::move(grade_stack);
    active_retouch_stroke_index_ = -1;
    selected_grade_node_index_ = new_selected_index;
    const int new_point_color_count =
        selectedGradeNode() == nullptr ? 0 : PointColorModel::count(selectedGradeNode()->fine);
    selected_point_color_index_ =
        new_point_color_count == 0 ? -1
        : selection_changed        ? 0
                            : std::clamp(selected_point_color_index_, 0, new_point_color_count - 1);
    if (selection_changed || new_point_color_count == 0) {
        clearPointColorScopeReference();
    }
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
    if (basic_changed || local_mask_changed || retouch_changed) {
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

QString EditController::gradeNodeHistoryKey(const QString& key) const {
    if (key.startsWith(QStringLiteral("retouch/")) || key.startsWith(QStringLiteral("geometry/"))) {
        return QStringLiteral("photo/%1").arg(key);
    }
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr
               ? key
               : QStringLiteral("grade_node/%1/%2").arg(grade_node->grade_node_id, key);
}

void EditController::finishActiveGesture() {
    active_parameter_gestures_.clear();
    first_interactive_frame_presented_ = false;
    cancelActivePreview(true);
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

void EditController::recordWorkingTransition(const QString& key, const BackendGradeStack& before) {
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

void EditController::parameterEdited(const QString& key, const BackendGradeStack& before) {
    if (!active_ || interactionLocked()) {
        return;
    }
    recordWorkingTransition(gradeNodeHistoryKey(key), before);
    notifyParametersChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(EDIT_PREVIEW_THROTTLE_MS);
}
