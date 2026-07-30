#include "edit_controller.hpp"
#include "edit_ai_mask_controller.hpp"
#include "edit_raw_foundation_controller.hpp"

#include <QCoreApplication>
#include <QEvent>

#include <cmath>
#include <initializer_list>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage edit_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
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

} // namespace

// Stable QML-facing composition and property-projection boundary. Editing
// behavior lives in responsibility-named edit_*_controller.cpp modules.
EditController::EditController(
    std::shared_ptr<DesktopBackend> backend,
    std::shared_ptr<EditPreviewStore> preview_store,
    std::shared_ptr<EditPreviewPresentationContext> preview_presentation_context,
    QObject* parent
) :
    QObject(parent), backend_(std::move(backend)), preview_store_(std::move(preview_store)),
    preview_presentation_context_(std::move(preview_presentation_context)), versions_(this),
    tone_curve_points_(this) {
    ai_mask_controller_ =
        std::make_unique<EditAiMaskController>(*this, backend_);
    raw_foundation_controller_ =
        std::make_unique<EditRawFoundationController>(*this, backend_);
    histogram_ = empty_histogram();
    before_histogram_ = empty_histogram();
    preview_debounce_.setSingleShot(true);
    detail_debounce_.setSingleShot(true);
    detail_warmup_debounce_.setSingleShot(true);
    autosave_debounce_.setSingleShot(true);
    connect(&preview_debounce_, &QTimer::timeout, this, &EditController::startPreviewRender);
    connect(
        this,
        &EditController::selectedGradeNodeChanged,
        this,
        &EditController::handleMaskSelectionChanged
    );
    connect(
        this,
        &EditController::parametersChanged,
        this,
        &EditController::handleMaskParametersChanged
    );
    connect(
        this,
        &EditController::sourceIdentityChanged,
        this,
        &EditController::handleMaskSourceIdentityChanged
    );
    connect(
        this,
        &EditController::sourceIdentityChanged,
        this,
        [this] { ai_mask_controller_->resetContext(); }
    );
    connect(
        this,
        &EditController::selectedGradeNodeChanged,
        this,
        [this] { ai_mask_controller_->resetContext(); }
    );
    connect(
        this,
        &EditController::parametersChanged,
        this,
        [this] { ai_mask_controller_->resetContext(); }
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
    connect(&detail_debounce_, &QTimer::timeout, this, &EditController::startDetailRender);
    connect(&autosave_debounce_, &QTimer::timeout, this, &EditController::startAutosave);
    connect(
        &detail_watcher_,
        &QFutureWatcher<EditDetailTaskResult>::finished,
        this,
        &EditController::finishDetailTask
    );
    connect(&detail_warmup_debounce_, &QTimer::timeout, this, &EditController::startDetailWarmup);
    connect(
        &detail_warmup_watcher_,
        &QFutureWatcher<EditDetailWarmupTaskResult>::finished,
        this,
        &EditController::finishDetailWarmupTask
    );
    if (auto* const application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
    refreshSharedGradeNodes();
}

EditController::~EditController() {
    raw_foundation_controller_.reset();
    ai_mask_controller_.reset();
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
    return state_running_ || current_rendering_ || before_rendering_ || detail_rendering_
           || (ai_mask_controller_ && ai_mask_controller_->busy());
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
    return pending_version_save_name_.has_value()
           || (state_running_ && state_task_kind_ != EditStateTaskKind::Autosave)
           || (ai_mask_controller_ && ai_mask_controller_->locksInteraction());
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
    return !autosaveFailed()
           && (autosave_requested_ || autosave_debounce_.isActive()
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

bool EditController::eventFilter(QObject* const watched, QEvent* const event) {
    if (watched == QCoreApplication::instance() && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    return QObject::eventFilter(watched, event);
}

void EditController::retranslateUi() {
    emit statusTextChanged();
    if (raw_foundation_controller_) {
        raw_foundation_controller_->retranslateUi();
    }
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
    const char* const label_source
) {
    if (!active_ || interactionLocked()) {
        return false;
    }
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        setStatusMessage(
            edit_message(QT_TRANSLATE_NOOP("EditController", "Select a Grade Node before editing"))
        );
        return false;
    }
    if (!grade_node->enabled) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Enable the selected Grade Node before editing its controls"
        )));
        return false;
    }
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        setStatusMessage(edit_message(
            QT_TRANSLATE_NOOP("EditController", "%1 is outside the supported preview range"),
            {LocalizedUiArgument::translatedText("EditController", label_source)}
        ));
        return false;
    }
    return true;
}
