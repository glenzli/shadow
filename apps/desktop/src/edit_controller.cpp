#include "edit_controller.hpp"
#include "ai_preferences.hpp"
#include "edit_ai_completion_controller.hpp"
#include "edit_ai_mask_controller.hpp"
#include "edit_auto_geometry_controller.hpp"
#include "edit_auto_start_controller.hpp"
#include "edit_paint_controller.hpp"
#include "edit_persistence_task_coordinator.hpp"
#include "edit_raw_foundation_controller.hpp"
#include "edit_retouch_sources.hpp"
#include "edit_subject_emphasis_controller.hpp"
#include "edit_targeted_curve_controller.hpp"

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
    AiPreferences* const ai_preferences,
    QObject* parent
) :
    QObject(parent), backend_(std::move(backend)), preview_store_(std::move(preview_store)),
    preview_presentation_context_(std::move(preview_presentation_context)),
    ai_preferences_(ai_preferences), persistence_state_(*this, [this] { startAutosave(); }),
    versions_(this), tone_curve_points_(this) {
    image_completion_controller_ = std::make_unique<EditAiCompletionController>(*this, backend_);
    ai_mask_controller_ = std::make_unique<EditAiMaskController>(*this, backend_);
    auto_start_controller_ = std::make_unique<EditAutoStartController>(*this, backend_);
    subject_emphasis_controller_ = std::make_unique<EditSubjectEmphasisController>(*this, backend_);
    paint_controller_ = std::make_unique<EditPaintController>(*this);
    targeted_curve_controller_ = std::make_unique<EditTargetedCurveController>(*this);
    retouch_sources_ = std::make_unique<EditRetouchSources>(*this);
    auto_geometry_controller_ = std::make_unique<EditAutoGeometryController>(*this);
    persistence_task_coordinator_ =
        std::make_unique<EditPersistenceTaskCoordinator>(*this, [this] { finishStateTask(); });
    raw_foundation_controller_ = std::make_unique<EditRawFoundationController>(*this, backend_);
    histogram_ = empty_histogram();
    before_histogram_ = empty_histogram();
    connect(this, &EditController::activeChanged, this, &EditController::variantActionsChanged);
    connect(this, &EditController::stateBusyChanged, this, &EditController::variantActionsChanged);
    connect(
        this,
        &EditController::autosavePendingChanged,
        this,
        &EditController::variantActionsChanged
    );
    preview_debounce_.setSingleShot(true);
    detail_debounce_.setSingleShot(true);
    detail_warmup_debounce_.setSingleShot(true);
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
    connect(this, &EditController::sourceIdentityChanged, this, [this] {
        ai_mask_controller_->resetContext();
        image_completion_controller_->resetContext();
    });
    connect(this, &EditController::selectedGradeNodeChanged, this, [this] {
        ai_mask_controller_->resetContext();
    });
    connect(this, &EditController::parametersChanged, this, [this] {
        ai_mask_controller_->resetContext();
        image_completion_controller_->resetContext();
    });
    connect(this, &EditController::sourceIdentityChanged, this, [this] {
        auto_geometry_controller_->resetContext();
    });
    connect(this, &EditController::parametersChanged, this, [this] {
        auto_geometry_controller_->resetContext();
    });
    connect(this, &EditController::cropToolActiveChanged, this, [this] {
        if (!crop_tool_active_) {
            auto_geometry_controller_->resetContext();
        } else {
            emit autoGeometryChanged();
        }
    });
    connect(this, &EditController::renderingChanged, this, &EditController::autoGeometryChanged);
    connect(this, &EditController::stateBusyChanged, this, &EditController::autoGeometryChanged);
    if (ai_preferences_ != nullptr) {
        connect(
            ai_preferences_,
            &AiPreferences::rawDenoiseExecutionAllowedChanged,
            this,
            &EditController::foundationAiDenoiseChanged
        );
        connect(
            ai_preferences_,
            &AiPreferences::subjectMaskExecutionAllowedChanged,
            this,
            &EditController::aiMaskPromptChanged
        );
        connect(
            ai_preferences_,
            &AiPreferences::imageCompletionExecutionAllowedChanged,
            this,
            &EditController::imageCompletionChanged
        );
    }
    connect(
        &preview_watcher_,
        &QFutureWatcher<EditPreviewTaskResult>::finished,
        this,
        &EditController::finishPreviewTask
    );
    connect(&detail_debounce_, &QTimer::timeout, this, &EditController::startDetailRender);
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
    retouch_sources_.reset();
    targeted_curve_controller_.reset();
    paint_controller_.reset();
    auto_geometry_controller_.reset();
    raw_foundation_controller_.reset();
    auto_start_controller_.reset();
    subject_emphasis_controller_.reset();
    ai_mask_controller_.reset();
    image_completion_controller_.reset();
    preview_debounce_.stop();
    detail_debounce_.stop();
    detail_warmup_debounce_.stop();
    persistence_state_.stopAutosaveDebounce();
    detail_render_token_ = backend_->beginEditDetailRequest();
    detail_warmup_token_ = detail_render_token_;
    persistence_task_coordinator_->waitForFinished();
    preview_watcher_.waitForFinished();
    detail_watcher_.waitForFinished();
    detail_warmup_watcher_.waitForFinished();
}

QObject* EditController::targetedCurve() const noexcept {
    return targeted_curve_controller_.get();
}

QObject* EditController::paint() const noexcept {
    return paint_controller_.get();
}

QObject* EditController::retouchSources() const noexcept {
    return retouch_sources_.get();
}

QObject* EditController::autoStart() const noexcept { return auto_start_controller_.get(); }

QObject* EditController::subjectEmphasis() const noexcept {
    return subject_emphasis_controller_.get();
}

bool EditController::active() const noexcept {
    return active_;
}

bool EditController::busy() const noexcept {
    return stateTaskRunning() || current_rendering_ || before_rendering_ || detail_rendering_
           || (auto_start_controller_ && auto_start_controller_->busy())
           || (subject_emphasis_controller_ && subject_emphasis_controller_->busy())
           || (ai_mask_controller_ && ai_mask_controller_->busy())
           || (image_completion_controller_ && image_completion_controller_->busy())
           || (auto_geometry_controller_ && auto_geometry_controller_->busy());
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
    return persistence_state_.hasPendingVersionSave() || persistence_state_.hasPendingVersionLoad()
           || (stateTaskRunning() && stateTaskKind() != EditStateTaskKind::Autosave)
           || (auto_start_controller_ && auto_start_controller_->applying())
           || (subject_emphasis_controller_ && subject_emphasis_controller_->active())
           || (ai_mask_controller_ && ai_mask_controller_->locksInteraction())
           || (image_completion_controller_ && image_completion_controller_->locksInteraction());
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

quint32 EditController::levelZeroWidth() const noexcept {
    return level_zero_width_;
}

quint32 EditController::levelZeroHeight() const noexcept {
    return level_zero_height_;
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
           && (persistence_state_.autosaveRequested() || persistence_state_.autosaveDebounceActive()
               || (stateTaskRunning() && stateTaskKind() == EditStateTaskKind::Autosave
                   && stateTaskFutureRunning()));
}

bool EditController::autosaveFailed() const noexcept {
    return persistence_state_.autosaveFailed();
}

QString EditController::autosaveErrorText() const {
    return persistence_state_.autosaveFailure().translated();
}

bool EditController::versionDraft() const noexcept {
    return version_draft_;
}

QString EditController::editBaseCommitId() const {
    return base_commit_id_;
}

QString EditController::durableWorkingCommitId() const {
    return durable_working_commit_id_;
}

QString EditController::activeVariantId() const {
    return active_variant_id_;
}

QVariantList EditController::photoVariants() const {
    return photo_variants_;
}

bool EditController::variantActionsEnabled() const noexcept {
    return active_ && !dirty_ && !autosavePending() && !stateBusy() && !version_draft_;
}

bool EditController::canUndo() const noexcept {
    return history_.canUndo() || (paint_controller_ && paint_controller_->strokeActive());
}

bool EditController::canRedo() const noexcept {
    return history_.canRedo() && !(paint_controller_ && paint_controller_->strokeActive());
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
    if (auto_geometry_controller_) {
        auto_geometry_controller_->retranslateUi();
    }
    if (raw_foundation_controller_) {
        raw_foundation_controller_->retranslateUi();
    }
    if (persistence_state_.autosaveFailed()) {
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
