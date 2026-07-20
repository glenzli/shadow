#include "edit_controller.hpp"

#include <QtConcurrent>

#include <QSize>

#include <cmath>
#include <exception>
#include <utility>

namespace {

constexpr std::uint32_t EDIT_PREVIEW_EDGE = 1'600;
constexpr std::uint8_t EDIT_PREVIEW_QUALITY = 88;
constexpr int EDIT_DEBOUNCE_MS = 140;

[[nodiscard]] EditStateTaskResult load_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Open;
    try {
        result.state = backend->photoEditState(photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult save_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const BackendBasicEditParameters parameters,
    const QString& version_name,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Save;
    try {
        result.state = backend->saveBasicEditVersion(
            photo_id,
            source_path,
            parameters,
            version_name
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditStateTaskResult checkout_state(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Checkout;
    try {
        result.state = backend->checkoutBasicEditVersion(
            photo_id,
            source_path,
            commit_id
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditPreviewTaskResult render_preview(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const BackendBasicEditParameters parameters,
    const quint64 photo_generation,
    const quint64 render_revision
) {
    EditPreviewTaskResult result;
    result.photo_generation = photo_generation;
    result.render_revision = render_revision;
    try {
        result.preview = backend->renderBasicEditPreview(
            photo_id,
            source_path,
            parameters,
            EDIT_PREVIEW_EDGE,
            EDIT_PREVIEW_QUALITY
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
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
      versions_(this) {
    preview_debounce_.setSingleShot(true);
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
}

EditController::~EditController() {
    preview_debounce_.stop();
    state_watcher_.waitForFinished();
    preview_watcher_.waitForFinished();
}

bool EditController::active() const noexcept {
    return active_;
}

bool EditController::busy() const noexcept {
    return state_running_ || preview_running_;
}

bool EditController::stateBusy() const noexcept {
    return state_running_;
}

bool EditController::rendering() const noexcept {
    return preview_running_;
}

bool EditController::dirty() const noexcept {
    return dirty_;
}

bool EditController::canUndo() const noexcept {
    return history_.canUndo();
}

bool EditController::canRedo() const noexcept {
    return history_.canRedo();
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

QString EditController::statusText() const {
    return status_text_;
}

double EditController::exposureStops() const noexcept {
    return parameters_.exposure_stops;
}

double EditController::contrastFactor() const noexcept {
    return parameters_.contrast_factor;
}

double EditController::redGain() const noexcept {
    return parameters_.red_channel_gain;
}

double EditController::greenGain() const noexcept {
    return parameters_.green_channel_gain;
}

double EditController::blueGain() const noexcept {
    return parameters_.blue_channel_gain;
}

double EditController::saturationFactor() const noexcept {
    return parameters_.saturation_factor;
}

QAbstractItemModel* EditController::versions() noexcept {
    return &versions_;
}

void EditController::setExposureStops(const double value) {
    if (parameters_.exposure_stops == value
        || !acceptParameter(value, -16.0, 16.0, QStringLiteral("Exposure"))) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    parameters_.exposure_stops = value;
    parameterEdited(QStringLiteral("exposure"), before);
}

void EditController::setContrastFactor(const double value) {
    if (parameters_.contrast_factor == value
        || !acceptParameter(value, 0.0, 8.0, QStringLiteral("Contrast"))) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    parameters_.contrast_factor = value;
    parameterEdited(QStringLiteral("contrast"), before);
}

void EditController::setRedGain(const double value) {
    if (parameters_.red_channel_gain == value
        || !acceptParameter(value, 0.000'001, 16.0, QStringLiteral("Red gain"))) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    parameters_.red_channel_gain = value;
    parameterEdited(QStringLiteral("red_gain"), before);
}

void EditController::setGreenGain(const double value) {
    if (parameters_.green_channel_gain == value
        || !acceptParameter(value, 0.000'001, 16.0, QStringLiteral("Green gain"))) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    parameters_.green_channel_gain = value;
    parameterEdited(QStringLiteral("green_gain"), before);
}

void EditController::setBlueGain(const double value) {
    if (parameters_.blue_channel_gain == value
        || !acceptParameter(value, 0.000'001, 16.0, QStringLiteral("Blue gain"))) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    parameters_.blue_channel_gain = value;
    parameterEdited(QStringLiteral("blue_gain"), before);
}

void EditController::setSaturationFactor(const double value) {
    if (parameters_.saturation_factor == value
        || !acceptParameter(value, 0.0, 8.0, QStringLiteral("Saturation"))) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    parameters_.saturation_factor = value;
    parameterEdited(QStringLiteral("saturation"), before);
}

void EditController::openPhoto(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_path,
    const QString& title
) {
    if (state_running_) {
        setStatusText(QStringLiteral("Finish the current version operation first"));
        return;
    }
    if (dirty_ && active_) {
        setStatusText(QStringLiteral("Save or revert the current changes before reopening a photo"));
        return;
    }
    if (active_ && photo_id == photo_id_ && source_path == source_path_) {
        setStatusText(QStringLiteral("This photo is already open in Precision"));
        return;
    }
    if (photo_id.isEmpty() || representation_id.isEmpty() || source_path.isEmpty()) {
        setStatusText(QStringLiteral("The selected Review item has no editable RAW source"));
        return;
    }

    ++photo_generation_;
    ++render_revision_;
    preview_debounce_.stop();
    preview_queued_ = false;
    photo_id_ = photo_id;
    representation_id_ = representation_id;
    source_path_ = source_path;
    title_ = title;
    versions_.replace({});
    committed_parameters_ = {};
    clearSessionHistory();
    setParameters({});
    if (!preview_source_.isEmpty()) {
        preview_source_.clear();
        emit previewSourceChanged();
    }
    preview_store_->clear(render_revision_);
    if (!active_) {
        active_ = true;
        emit activeChanged();
    }
    emit titleChanged();
    emit sourcePathChanged();
    setStateRunning(true);
    setStatusText(QStringLiteral("Loading non-destructive edit history…"));
    state_watcher_.setFuture(QtConcurrent::run(
        load_state,
        backend_,
        photo_id_,
        source_path_,
        photo_generation_
    ));
}

void EditController::closePhoto() {
    if (dirty_) {
        setStatusText(QStringLiteral("Save or revert the current changes before closing Precision"));
        return;
    }
    if (!active_) {
        return;
    }
    clearSessionHistory();
    active_ = false;
    emit activeChanged();
    emit historyChanged();
}

void EditController::beginParameterEdit(const QString& parameter_key) {
    if (!active_ || state_running_ || parameter_key.isEmpty()) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.beginGesture(parameter_key.toStdString(), parameters_);
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
    history_.endGesture(parameter_key.toStdString(), parameters_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::undo() {
    if (!active_ || state_running_) {
        return;
    }
    const auto restored = history_.undo(parameters_);
    emit historyChanged();
    if (!restored) {
        return;
    }
    setParameters(*restored);
    schedulePreview(0);
    setStatusText(QStringLiteral("Undid the last session adjustment"));
}

void EditController::redo() {
    if (!active_ || state_running_) {
        return;
    }
    const auto restored = history_.redo(parameters_);
    emit historyChanged();
    if (!restored) {
        return;
    }
    setParameters(*restored);
    schedulePreview(0);
    setStatusText(QStringLiteral("Redid the last session adjustment"));
}

void EditController::resetEdits() {
    if (!active_ || state_running_) {
        return;
    }
    const BackendBasicEditParameters neutral;
    if (parameters_ == neutral) {
        return;
    }
    const BackendBasicEditParameters before = parameters_;
    setParameters(neutral);
    recordWorkingTransition(QStringLiteral("reset"), before);
    schedulePreview(0);
}

void EditController::revertEdits() {
    if (!active_ || state_running_) {
        return;
    }
    if (parameters_ != committed_parameters_) {
        const BackendBasicEditParameters before = parameters_;
        setParameters(committed_parameters_);
        recordWorkingTransition(QStringLiteral("revert"), before);
        schedulePreview(0);
    }
    setStatusText(QStringLiteral("Restored the current saved version"));
}

void EditController::saveVersion(const QString& version_name) {
    const QString name = version_name.trimmed();
    if (!active_ || state_running_) {
        return;
    }
    if (name.isEmpty()) {
        setStatusText(QStringLiteral("Enter a name for this version"));
        return;
    }
    history_.finishGesture(parameters_);
    emit historyChanged();
    setStateRunning(true);
    setStatusText(QStringLiteral("Saving immutable version “%1”…").arg(name));
    state_watcher_.setFuture(QtConcurrent::run(
        save_state,
        backend_,
        photo_id_,
        source_path_,
        parameters_,
        name,
        photo_generation_
    ));
}

void EditController::checkoutVersion(const QString& commit_id) {
    if (!active_ || state_running_ || commit_id.isEmpty()) {
        return;
    }
    if (dirty_) {
        setStatusText(QStringLiteral("Save or revert current changes before checking out another version"));
        return;
    }
    setStateRunning(true);
    setStatusText(QStringLiteral("Checking out an immutable version…"));
    state_watcher_.setFuture(QtConcurrent::run(
        checkout_state,
        backend_,
        photo_id_,
        source_path_,
        commit_id,
        photo_generation_
    ));
}

void EditController::finishStateTask() {
    EditStateTaskResult result = state_watcher_.result();
    setStateRunning(false);
    if (result.photo_generation != photo_generation_) {
        return;
    }
    if (!result.error.isEmpty()) {
        if (result.kind == EditStateTaskKind::Open && active_) {
            active_ = false;
            emit activeChanged();
        }
        setStatusText(QStringLiteral("Version operation failed · %1").arg(result.error));
        if (preview_queued_) {
            preview_debounce_.start(0);
        }
        return;
    }
    applyState(std::move(result.state));
    switch (result.kind) {
    case EditStateTaskKind::Open:
        setStatusText(QStringLiteral("Edit history ready · rendering scene-linear preview"));
        schedulePreview(0);
        break;
    case EditStateTaskKind::Save:
        setStatusText(QStringLiteral("Version saved · the previous head remains available"));
        break;
    case EditStateTaskKind::Checkout:
        setStatusText(QStringLiteral("Version checked out · new edits will branch from here"));
        schedulePreview(0);
        break;
    }
    if (preview_queued_ && !preview_debounce_.isActive()) {
        preview_debounce_.start(0);
    }
}

void EditController::finishPreviewTask() {
    EditPreviewTaskResult result = preview_watcher_.result();
    setPreviewRunning(false);
    const bool current = result.photo_generation == photo_generation_
        && result.render_revision == render_revision_;
    if (current && !result.error.isEmpty()) {
        setStatusText(QStringLiteral("Preview render failed · %1").arg(result.error));
    } else if (current) {
        const QSize dimensions(
            static_cast<int>(result.preview.width),
            static_cast<int>(result.preview.height)
        );
        preview_store_->publish(
            std::move(result.preview.bytes),
            dimensions,
            result.render_revision
        );
        preview_source_ = QStringLiteral("image://shadow-edit/current?generation=%1")
                              .arg(result.render_revision);
        emit previewSourceChanged();
        setStatusText(
            dirty_ ? QStringLiteral("Unsaved changes · preview is current")
                   : QStringLiteral("Version and preview are current")
        );
    }
    if (preview_queued_ || result.render_revision != render_revision_) {
        preview_queued_ = false;
        schedulePreview(0);
    }
}

void EditController::startPreviewRender() {
    if (!active_ || state_running_) {
        preview_queued_ = active_;
        return;
    }
    if (preview_running_) {
        preview_queued_ = true;
        return;
    }
    setPreviewRunning(true);
    preview_queued_ = false;
    setStatusText(QStringLiteral("Rendering bounded scene-linear preview…"));
    preview_watcher_.setFuture(QtConcurrent::run(
        render_preview,
        backend_,
        photo_id_,
        source_path_,
        parameters_,
        photo_generation_,
        render_revision_
    ));
}

void EditController::applyState(BackendPhotoEditState state) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
        setStatusText(QStringLiteral("Catalog returned edit state for a different photo"));
        return;
    }
    committed_parameters_ = state.parameters;
    setParameters(state.parameters);
    clearSessionHistory();
    versions_.replace(std::move(state.versions));
}

void EditController::setParameters(
    const BackendBasicEditParameters parameters
) {
    const bool changed = parameters_ != parameters;
    parameters_ = parameters;
    if (changed) {
        emit parametersChanged();
    }
    setDirty(parameters_ != committed_parameters_);
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
    const BackendBasicEditParameters& before
) {
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    history_.record(key.toStdString(), before, parameters_);
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
}

void EditController::schedulePreview(const int delay_ms) {
    if (!active_) {
        return;
    }
    ++render_revision_;
    if (preview_running_) {
        preview_queued_ = true;
    }
    preview_debounce_.start(delay_ms);
}

void EditController::setStatusText(QString status) {
    if (status_text_ == status) {
        return;
    }
    status_text_ = std::move(status);
    emit statusTextChanged();
}

void EditController::setDirty(const bool dirty) {
    if (dirty_ == dirty) {
        return;
    }
    dirty_ = dirty;
    emit dirtyChanged();
}

void EditController::setStateRunning(const bool running) {
    if (state_running_ == running) {
        return;
    }
    const bool previous_busy = busy();
    state_running_ = running;
    emit stateBusyChanged();
    emitBusyChange(previous_busy);
}

void EditController::setPreviewRunning(const bool running) {
    if (preview_running_ == running) {
        return;
    }
    const bool previous_busy = busy();
    preview_running_ = running;
    emit renderingChanged();
    emitBusyChange(previous_busy);
}

void EditController::emitBusyChange(const bool previous_busy) {
    if (previous_busy != busy()) {
        emit busyChanged();
    }
}

void EditController::parameterEdited(
    const QString& key,
    const BackendBasicEditParameters& before
) {
    if (!active_ || state_running_) {
        return;
    }
    recordWorkingTransition(key, before);
    emit parametersChanged();
    setDirty(parameters_ != committed_parameters_);
    schedulePreview(EDIT_DEBOUNCE_MS);
}

bool EditController::acceptParameter(
    const double value,
    const double minimum,
    const double maximum,
    const QString& label
) {
    if (!active_ || state_running_) {
        return false;
    }
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        setStatusText(
            QStringLiteral("%1 is outside the supported preview range").arg(label)
        );
        return false;
    }
    return true;
}
