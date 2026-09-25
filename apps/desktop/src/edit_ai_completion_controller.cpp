#include "edit_ai_completion_controller.hpp"

#include "ai_preferences.hpp"
#include "edit_ai_mask_controller.hpp"
#include "edit_controller.hpp"

#include <QBuffer>
#include <QImage>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage completion_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] EditImageCompletionExecutionResult execute_completion(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const BackendImageCompletionRequest request
) {
    EditImageCompletionExecutionResult result;
    result.result.job_token = request.job_token;
    result.result.generation = request.generation;
    try {
        result.result = backend->executeImageCompletionJob(photo_id, source_path, request);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditImageCompletionApplyResult apply_completion(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const BackendImageCompletionApplyRequest request
) {
    EditImageCompletionApplyResult result;
    try {
        result.state = backend->applyImageCompletionProposal(photo_id, source_path, request);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] QString candidate_source(const BackendImageCompletionResult& result) {
    const std::uint64_t expected = static_cast<std::uint64_t>(result.preview_width)
                                   * static_cast<std::uint64_t>(result.preview_height) * 4U;
    if (result.preview_width == 0U || result.preview_height == 0U
        || expected != static_cast<std::uint64_t>(result.preview_rgba8.size())) {
        return {};
    }
    QImage image{
        reinterpret_cast<const uchar*>(result.preview_rgba8.constData()),
        static_cast<int>(result.preview_width),
        static_cast<int>(result.preview_height),
        static_cast<qsizetype>(result.preview_width) * 4,
        QImage::Format_RGBA8888,
    };
    if (image.isNull()) {
        return {};
    }
    QByteArray png;
    QBuffer buffer{&png};
    if (!buffer.open(QIODevice::WriteOnly) || !image.copy().save(&buffer, "PNG")) {
        return {};
    }
    return QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(png.toBase64()));
}

} // namespace

EditAiCompletionController::EditAiCompletionController(
    EditController& owner,
    std::shared_ptr<DesktopBackend> backend
) : owner_(owner), backend_(std::move(backend)) {
    QObject::connect(
        &execution_watcher_,
        &QFutureWatcher<EditImageCompletionExecutionResult>::finished,
        &owner_,
        [this] { finishExecution(); }
    );
    QObject::connect(
        &apply_watcher_,
        &QFutureWatcher<EditImageCompletionApplyResult>::finished,
        &owner_,
        [this] { finishApply(); }
    );
    pending_generation_retry_.setSingleShot(true);
    QObject::connect(&pending_generation_retry_, &QTimer::timeout, &owner_, [this] {
        tryStartPendingGeneration();
    });
    const auto retry = [this] {
        if (generation_pending_) {
            pending_generation_retry_.start(0);
        }
    };
    QObject::connect(&owner_, &EditController::autosavePendingChanged, &owner_, retry);
    QObject::connect(&owner_, &EditController::stateBusyChanged, &owner_, retry);
}

EditAiCompletionController::~EditAiCompletionController() {
    pending_generation_retry_.stop();
    QObject::disconnect(&execution_watcher_, nullptr, &owner_, nullptr);
    QObject::disconnect(&apply_watcher_, nullptr, &owner_, nullptr);
    if (active_job_token_ != 0) {
        try {
            backend_->cancelImageCompletionJob(active_job_token_);
        } catch (...) {}
    }
    retireCandidate();
    execution_watcher_.waitForFinished();
    if (execution_watcher_.future().isValid()) {
        const auto result = execution_watcher_.result();
        if (result.result.terminal == BackendImageCompletionTerminal::Staged) {
            discardProposal(result.result.proposal_token);
        }
    }
    apply_watcher_.waitForFinished();
}

bool EditAiCompletionController::active() const noexcept {
    return active_;
}

bool EditAiCompletionController::busy() const noexcept {
    return generation_pending_ || execution_watcher_.isRunning() || apply_in_flight_;
}

bool EditAiCompletionController::locksInteraction() const noexcept {
    return active_ || busy();
}

bool EditAiCompletionController::canGenerate() const noexcept {
    return active_ && !busy() && (hasPaintedPoint() || refresh_region_index_ >= 0)
           && (!owner_.liquifyNodeMaterialized() || !owner_.liquifyNodeEnabled());
}

bool EditAiCompletionController::hasCandidate() const noexcept {
    return candidate_proposal_token_ != 0 && !candidate_source_.isEmpty();
}

QString EditAiCompletionController::candidateSource() const {
    return candidate_source_;
}

QVariantList EditAiCompletionController::brushPoints() const {
    QVariantList result;
    result.reserve(points_.size());
    for (const auto& point : points_) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("x"), point.x},
                {QStringLiteral("y"), point.y},
                {QStringLiteral("radius"), point.radius},
                {QStringLiteral("erase"), point.erase},
                {QStringLiteral("strokeId"), point.stroke_id},
            }
        );
    }
    return result;
}

double EditAiCompletionController::brushRadius() const noexcept {
    return brush_radius_;
}

bool EditAiCompletionController::eraseMode() const noexcept {
    return erase_mode_;
}

bool EditAiCompletionController::begin() {
    if (!owner_.active_ || owner_.interactionLocked()) {
        return false;
    }
    if (owner_.ai_mask_controller_) {
        owner_.ai_mask_controller_->resetContext();
    }
    owner_.selectImageCompletionNode();
    active_ = true;
    generation_pending_ = false;
    refresh_region_index_ = -1;
    points_.clear();
    retireCandidate();
    context_ = Context{
        .photo_id = owner_.photo_id_,
        .source_path = owner_.source_path_,
        .photo_generation = owner_.photo_generation_,
    };
    owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
        "EditController",
        "AI Completion · paint the area to replace, then generate"
    )));
    publishStateChange();
    return true;
}

bool EditAiCompletionController::refreshRegion(const int index) {
    if (index < 0 || index >= owner_.grade_stack_.image_completions.size() || busy()) {
        return false;
    }
    if (!active_ && !begin()) {
        return false;
    }
    if (!contextIsCurrent()) {
        return false;
    }
    retireCandidate();
    points_.clear();
    refresh_region_index_ = index;
    generate();
    return true;
}

std::uint32_t EditAiCompletionController::beginStroke() {
    if (!active_ || busy() || !contextIsCurrent()
        || next_stroke_id_ == std::numeric_limits<std::uint32_t>::max()) {
        return 0;
    }
    return ++next_stroke_id_;
}

void EditAiCompletionController::appendPoint(
    const double x,
    const double y,
    const std::uint32_t stroke_id
) {
    if (!active_ || busy() || stroke_id == 0 || !contextIsCurrent() || !std::isfinite(x)
        || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
        return;
    }
    if (points_.size() >= 8'192) {
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "AI Completion selection is too complex")
        ));
        return;
    }
    retireCandidate();
    points_.push_back({
        .x = x,
        .y = y,
        .radius = brush_radius_,
        .erase = erase_mode_,
        .stroke_id = stroke_id,
    });
    publishStateChange();
}

void EditAiCompletionController::undoStroke() {
    if (!active_ || busy() || points_.isEmpty()) {
        return;
    }
    retireCandidate();
    const auto stroke_id = points_.constLast().stroke_id;
    while (!points_.isEmpty() && points_.constLast().stroke_id == stroke_id) {
        points_.removeLast();
    }
    publishStateChange();
}

void EditAiCompletionController::clearSelection() {
    if (!active_ || busy() || points_.isEmpty()) {
        return;
    }
    retireCandidate();
    points_.clear();
    publishStateChange();
}

void EditAiCompletionController::generate() {
    if (!active_ || busy() || (!hasPaintedPoint() && refresh_region_index_ < 0)
        || !contextIsCurrent()) {
        return;
    }
    if (!owner_.imageCompletionExecutionAllowed()) {
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "AI Completion requires local execution permission")
        ));
        publishStateChange();
        return;
    }
    if (owner_.liquifyNodeMaterialized() && owner_.liquifyNodeEnabled()) {
        owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI Completion is before Liquify · bypass Liquify before generating"
        )));
        publishStateChange();
        return;
    }
    generation_pending_ = true;
    publishStateChange();
    tryStartPendingGeneration();
}

void EditAiCompletionController::retry() {
    if (!active_ || busy()) {
        return;
    }
    retireCandidate();
    generate();
}

void EditAiCompletionController::tryStartPendingGeneration() {
    if (!generation_pending_ || !active_ || execution_watcher_.isRunning() || apply_in_flight_) {
        return;
    }
    if (!contextIsCurrent() || (!hasPaintedPoint() && refresh_region_index_ < 0)) {
        generation_pending_ = false;
        publishStateChange();
        return;
    }
    if (owner_.autosaveFailed()) {
        generation_pending_ = false;
        owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI Completion cannot continue until the current adjustments are saved"
        )));
        publishStateChange();
        return;
    }
    if (owner_.dirty_ || owner_.stateTaskRunning()) {
        if (owner_.dirty_ && !owner_.stateTaskRunning()) {
            owner_.persistence_state_.requestAutosave();
            owner_.startAutosave();
        }
        owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI Completion is waiting for current adjustments to finish saving"
        )));
        return;
    }
    startGeneration();
}

void EditAiCompletionController::startGeneration() {
    if (!owner_.imageCompletionExecutionAllowed()) {
        generation_pending_ = false;
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "AI Completion requires local execution permission")
        ));
        publishStateChange();
        return;
    }
    std::uint64_t job_token = 0;
    try {
        job_token = backend_->beginImageCompletionJob();
    } catch (const std::exception& error) {
        generation_pending_ = false;
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "Could not start AI Completion · %1"),
            {QString::fromUtf8(error.what())}
        ));
        publishStateChange();
        return;
    }
    retireCandidate();
    active_job_token_ = job_token;
    generation_++;
    submitted_grade_stack_ = owner_.grade_stack_;
    submitted_base_commit_id_ = owner_.base_commit_id_;
    BackendImageCompletionRequest request{
        .job_token = job_token,
        .generation = generation_,
        .base_commit_id = submitted_base_commit_id_,
        .grade_stack = submitted_grade_stack_,
        .points = points_,
        .refresh_region_index = refresh_region_index_,
    };
    generation_pending_ = false;
    owner_.setStatusMessage(completion_message(
        QT_TRANSLATE_NOOP("EditController", "AI Completion · generating a local candidate…")
    ));
    execution_watcher_.setFuture(
        QtConcurrent::run(
            execute_completion,
            backend_,
            context_->photo_id,
            context_->source_path,
            std::move(request)
        )
    );
    publishStateChange();
}

void EditAiCompletionController::applyCandidate() {
    if (!hasCandidate() || busy() || !contextIsCurrent()
        || owner_.grade_stack_ != submitted_grade_stack_
        || owner_.base_commit_id_ != submitted_base_commit_id_) {
        return;
    }
    applying_proposal_token_ = candidate_proposal_token_;
    candidate_proposal_token_ = 0;
    candidate_source_.clear();
    apply_in_flight_ = true;
    BackendImageCompletionApplyRequest request{
        .proposal_token = applying_proposal_token_,
        .generation = candidate_generation_,
        .base_commit_id = owner_.base_commit_id_,
        .expected_working_commit_id = owner_.durable_working_commit_id_,
        .grade_stack = owner_.grade_stack_,
        .replace_region_index = refresh_region_index_,
    };
    apply_watcher_.setFuture(
        QtConcurrent::run(
            apply_completion,
            backend_,
            context_->photo_id,
            context_->source_path,
            std::move(request)
        )
    );
    owner_.setStatusMessage(completion_message(
        QT_TRANSLATE_NOOP("EditController", "AI Completion · applying candidate…")
    ));
    publishStateChange();
}

void EditAiCompletionController::cancel() {
    if (!active_ || apply_in_flight_) {
        return;
    }
    generation_pending_ = false;
    if (active_job_token_ != 0) {
        try {
            backend_->cancelImageCompletionJob(active_job_token_);
        } catch (...) {}
    }
    retireCandidate();
    points_.clear();
    refresh_region_index_ = -1;
    active_ = false;
    context_.reset();
    owner_.setStatusMessage(
        completion_message(QT_TRANSLATE_NOOP("EditController", "AI Completion cancelled"))
    );
    publishStateChange();
}

void EditAiCompletionController::setBrushRadius(const double radius) {
    if (!std::isfinite(radius)) {
        return;
    }
    const double bounded = std::clamp(radius, 0.002, 0.25);
    if (brush_radius_ == bounded) {
        return;
    }
    brush_radius_ = bounded;
    publishStateChange();
}

void EditAiCompletionController::setEraseMode(const bool erase) {
    if (erase_mode_ == erase) {
        return;
    }
    erase_mode_ = erase;
    publishStateChange();
}

void EditAiCompletionController::resetContext() {
    if (!active_ && !busy() && !hasCandidate()) {
        return;
    }
    generation_pending_ = false;
    if (active_job_token_ != 0) {
        try {
            backend_->cancelImageCompletionJob(active_job_token_);
        } catch (...) {}
    }
    retireCandidate();
    points_.clear();
    refresh_region_index_ = -1;
    active_ = false;
    context_.reset();
    publishStateChange();
}

void EditAiCompletionController::finishExecution() {
    const auto task = execution_watcher_.result();
    active_job_token_ = 0;
    if (!task.error.isEmpty()) {
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "AI Completion failed · %1"),
            {task.error}
        ));
        publishStateChange();
        return;
    }
    const bool current = task.result.generation == generation_ && contextIsCurrent()
                         && owner_.grade_stack_ == submitted_grade_stack_
                         && owner_.base_commit_id_ == submitted_base_commit_id_;
    if (!current) {
        if (task.result.terminal == BackendImageCompletionTerminal::Staged) {
            discardProposal(task.result.proposal_token);
        }
        publishStateChange();
        return;
    }
    switch (task.result.terminal) {
    case BackendImageCompletionTerminal::Cancelled:
        owner_.setStatusMessage(
            completion_message(QT_TRANSLATE_NOOP("EditController", "AI Completion cancelled"))
        );
        publishStateChange();
        return;
    case BackendImageCompletionTerminal::Unavailable:
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP(
                "EditController",
                "AI Completion is unavailable · check Infer Runtime and the local model · %1"
            ),
            {task.result.detail}
        ));
        publishStateChange();
        return;
    case BackendImageCompletionTerminal::Failed:
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "AI Completion failed · %1"),
            {task.result.detail}
        ));
        publishStateChange();
        return;
    case BackendImageCompletionTerminal::Staged:
        break;
    }
    const QString source = candidate_source(task.result);
    if (task.result.proposal_token == 0 || source.isEmpty()) {
        discardProposal(task.result.proposal_token);
        owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI Completion returned an invalid candidate preview"
        )));
        publishStateChange();
        return;
    }
    retireCandidate();
    candidate_proposal_token_ = task.result.proposal_token;
    candidate_generation_ = task.result.generation;
    candidate_source_ = source;
    owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
        "EditController",
        "AI Completion candidate ready · apply, retry, or cancel"
    )));
    publishStateChange();
}

void EditAiCompletionController::finishApply() {
    auto task = apply_watcher_.result();
    apply_in_flight_ = false;
    applying_proposal_token_ = 0;
    if (!task.error.isEmpty()) {
        owner_.setStatusMessage(completion_message(
            QT_TRANSLATE_NOOP("EditController", "Could not apply AI Completion · %1"),
            {task.error}
        ));
        publishStateChange();
        return;
    }
    if (!contextIsCurrent()) {
        active_ = false;
        context_.reset();
        publishStateChange();
        return;
    }
    const BackendGradeStack before = owner_.grade_stack_;
    active_ = false;
    points_.clear();
    refresh_region_index_ = -1;
    context_.reset();
    owner_.applyImageCompletionState(std::move(task.state), before);
    owner_.setStatusMessage(completion_message(QT_TRANSLATE_NOOP(
        "EditController",
        "AI Completion applied · the region is now managed by the photo node"
    )));
    publishStateChange();
}

void EditAiCompletionController::retireCandidate() noexcept {
    const auto token = candidate_proposal_token_;
    candidate_proposal_token_ = 0;
    candidate_generation_ = 0;
    candidate_source_.clear();
    discardProposal(token);
}

void EditAiCompletionController::discardProposal(const std::uint64_t token) const noexcept {
    if (token == 0) {
        return;
    }
    try {
        backend_->discardImageCompletionProposal(token);
    } catch (...) {}
}

void EditAiCompletionController::publishStateChange() {
    emit owner_.imageCompletionChanged();
    emit owner_.busyChanged();
    emit owner_.stateBusyChanged();
    emit owner_.gradeNodeActionsChanged();
}

bool EditAiCompletionController::contextIsCurrent() const noexcept {
    return active_ && context_ && owner_.photo_generation_ == context_->photo_generation
           && owner_.photo_id_ == context_->photo_id
           && owner_.source_path_ == context_->source_path;
}

bool EditAiCompletionController::hasPaintedPoint() const noexcept {
    return std::ranges::any_of(points_, [](const auto& point) { return !point.erase; });
}

bool EditController::imageCompletionActive() const noexcept {
    return image_completion_controller_ && image_completion_controller_->active();
}

bool EditController::imageCompletionBusy() const noexcept {
    return image_completion_controller_ && image_completion_controller_->busy();
}

bool EditController::imageCompletionCanGenerate() const noexcept {
    return image_completion_controller_ && image_completion_controller_->canGenerate();
}

bool EditController::imageCompletionExecutionAllowed() const noexcept {
    return ai_preferences_ == nullptr || ai_preferences_->imageCompletionExecutionAllowed();
}

bool EditController::imageCompletionHasCandidate() const noexcept {
    return image_completion_controller_ && image_completion_controller_->hasCandidate();
}

QString EditController::imageCompletionCandidateSource() const {
    return image_completion_controller_ ? image_completion_controller_->candidateSource()
                                        : QString{};
}

QVariantList EditController::imageCompletionBrushPoints() const {
    return image_completion_controller_ ? image_completion_controller_->brushPoints()
                                        : QVariantList{};
}

double EditController::imageCompletionBrushRadius() const noexcept {
    return image_completion_controller_ ? image_completion_controller_->brushRadius() : 0.04;
}

bool EditController::imageCompletionEraseMode() const noexcept {
    return image_completion_controller_ && image_completion_controller_->eraseMode();
}

QVariantList EditController::imageCompletionRegions() const {
    QVariantList result;
    result.reserve(grade_stack_.image_completions.size());
    for (qsizetype index = 0; index < grade_stack_.image_completions.size(); ++index) {
        const auto& region = grade_stack_.image_completions.at(index);
        result.push_back(
            QVariantMap{
                {QStringLiteral("index"), index},
                {QStringLiteral("enabled"), region.enabled},
                {QStringLiteral("strength"), region.strength},
                {QStringLiteral("modelBuild"), region.model_build},
                {QStringLiteral("provider"), region.provider},
                {QStringLiteral("executionProvider"), region.actual_execution_provider},
                {QStringLiteral("sourceRecipe"), region.source_recipe_blake3},
                {QStringLiteral("preGrade"), region.pre_grade},
            }
        );
    }
    return result;
}

bool EditController::imageCompletionNodeMaterialized() const noexcept {
    return !grade_stack_.image_completions.isEmpty();
}

bool EditController::imageCompletionNodeEnabled() const noexcept {
    return grade_stack_.image_completion_enabled;
}

bool EditController::beginImageCompletion() {
    return image_completion_controller_ && image_completion_controller_->begin();
}

bool EditController::refreshImageCompletionRegion(const int index) {
    return image_completion_controller_ && image_completion_controller_->refreshRegion(index);
}

quint32 EditController::beginImageCompletionStroke() {
    return image_completion_controller_ ? image_completion_controller_->beginStroke() : 0;
}

void EditController::addImageCompletionBrushPoint(
    const double normalized_x,
    const double normalized_y,
    const quint32 stroke_id
) {
    if (image_completion_controller_) {
        image_completion_controller_->appendPoint(normalized_x, normalized_y, stroke_id);
    }
}

void EditController::undoImageCompletionStroke() {
    if (image_completion_controller_) {
        image_completion_controller_->undoStroke();
    }
}

void EditController::clearImageCompletionSelection() {
    if (image_completion_controller_) {
        image_completion_controller_->clearSelection();
    }
}

void EditController::generateImageCompletion() {
    if (image_completion_controller_) {
        image_completion_controller_->generate();
    }
}

void EditController::retryImageCompletion() {
    if (image_completion_controller_) {
        image_completion_controller_->retry();
    }
}

void EditController::applyImageCompletionCandidate() {
    if (image_completion_controller_) {
        image_completion_controller_->applyCandidate();
    }
}

void EditController::cancelImageCompletion() {
    if (image_completion_controller_) {
        image_completion_controller_->cancel();
    }
}

void EditController::setImageCompletionBrushRadius(const double radius) {
    if (image_completion_controller_) {
        image_completion_controller_->setBrushRadius(radius);
    }
}

void EditController::setImageCompletionEraseMode(const bool erase) {
    if (image_completion_controller_) {
        image_completion_controller_->setEraseMode(erase);
    }
}

void EditController::setImageCompletionExecutionAllowed(const bool allowed) {
    if (ai_preferences_ != nullptr) {
        ai_preferences_->setImageCompletionExecutionAllowed(allowed);
    }
}

void EditController::setImageCompletionNodeEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.image_completions.isEmpty()
        || grade_stack_.image_completion_enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.image_completion_enabled = enabled;
    parameterEdited(QStringLiteral("image_completion/enabled"), before);
}

void EditController::setImageCompletionRegionEnabled(const int index, const bool enabled) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.image_completions.size()
        || grade_stack_.image_completions[index].enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.image_completions[index].enabled = enabled;
    parameterEdited(QStringLiteral("image_completion/region/%1/enabled").arg(index), before);
}

void EditController::setImageCompletionRegionStrength(const int index, const double strength) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.image_completions.size() || !std::isfinite(strength)) {
        return;
    }
    const double bounded = std::clamp(strength, 0.0, 1.0);
    if (grade_stack_.image_completions[index].strength == bounded) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.image_completions[index].strength = bounded;
    parameterEdited(QStringLiteral("image_completion/region/%1/strength").arg(index), before);
}

void EditController::removeImageCompletionRegion(const int index) {
    if (!active_ || interactionLocked() || index < 0
        || index >= grade_stack_.image_completions.size()) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.image_completions.removeAt(index);
    if (grade_stack_.image_completions.isEmpty()) {
        grade_stack_.image_completion_enabled = true;
    }
    parameterEdited(QStringLiteral("image_completion/region/%1/remove").arg(index), before);
}

void EditController::applyImageCompletionState(
    BackendPhotoEditState state,
    const BackendGradeStack& before
) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
        return;
    }
    const bool could_undo = canUndo();
    const bool could_redo = canRedo();
    setVersionDraft(false);
    persistence_state_.stopAutosaveDebounce();
    clearAutosaveFailure();
    if (persistence_state_.clearAutosaveRequest()) {
        emit autosavePendingChanged();
    }
    const QString before_base_commit_id = base_commit_id_;
    base_commit_id_ = state.base_commit_id;
    durable_working_commit_id_ = state.base_commit_id;
    committed_grade_stack_ = state.grade_stack;
    setGradeStack(std::move(state.grade_stack));
    history_.record(
        "image_completion/apply",
        {before, before_base_commit_id},
        {grade_stack_, base_commit_id_}
    );
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
    working_revision_ = 0;
    persistence_state_.resetAutosaveSnapshot();
    versions_.replace(std::move(state.versions));
    setDirty(false);
    selectImageCompletionNode();
    schedulePreview(0);
}
