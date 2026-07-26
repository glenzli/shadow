#include "edit_controller.hpp"

#include "edit_point_color_model.hpp"
#include "edit_stack.hpp"

#include <QtConcurrent>

#include <QCoreApplication>
#include <QEvent>

#include <QSize>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <utility>

namespace {

// This is Shadow's resident editing proxy, not the full-resolution detail
// source. 768px is too aggressive for modern high-resolution RAWs: Bayer
// phase-preserving downsampling can turn a Z9 frame into a ~690px proxy.
// 1536px is still practical for interactive grading while preserving enough
// texture, edges and colour detail for a useful editing view.
constexpr int EDIT_PREVIEW_THROTTLE_MS = 16;
[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"EditController", source, arguments};
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
      tone_curve_points_(this),
      node_mask_asset_settings_(std::make_unique<QSettings>()) {
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
  loadNodeMaskAssets();
  refreshSharedGradeNodes();
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

bool EditController::fullResolutionPreparing() const noexcept {
    return full_resolution_preparing_;
}

bool EditController::fullResolutionReady() const noexcept {
    return full_resolution_ready_;
}

quint64 EditController::fullResolutionRetainedBytes() const noexcept {
    return full_resolution_retained_bytes_;
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

QAbstractItemModel* EditController::versions() noexcept {
    return &versions_;
}

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
            QT_TRANSLATE_NOOP(
                "EditController",
                "Could not reset adjustments · %1"
            ),
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
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Reset all adjustments"
    )));
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
    const bool retouch_changed = grade_stack_.retouch_spots != grade_stack.retouch_spots
        || grade_stack_.retouch_strokes != grade_stack.retouch_strokes;
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
    active_retouch_stroke_index_ = -1;
    selected_grade_node_index_ = new_selected_index;
    const int new_point_color_count = selectedGradeNode() == nullptr
        ? 0 : PointColorModel::count(selectedGradeNode()->fine);
    selected_point_color_index_ = new_point_color_count == 0
        ? -1
        : selection_changed ? 0
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
    if (basic_changed || retouch_changed) {
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
    if (key.startsWith(QStringLiteral("retouch/"))
        || key.startsWith(QStringLiteral("geometry/"))) {
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

void EditController::notifyParametersChanged() {
    if (point_color_scope_active_ && !pointColorScopeAvailable()) {
        point_color_scope_active_ = false;
        refreshCurrentDisplayScope();
        emit pointColorScopeChanged();
    } else if (point_color_scope_active_) {
        // Selecting another Point Color or changing its hue interval should
        // update the diagnostic immediately. The rendered preview stays
        // untouched; its normal async replacement is still scheduled by the
        // edit mutation that reached this notification.
        refreshCurrentDisplayScope();
    }
    ++parameter_revision_;
    emit parametersChanged();
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
