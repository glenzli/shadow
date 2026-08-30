#include "edit_ai_mask_controller.hpp"

#include "ai_preferences.hpp"
#include "edit_controller.hpp"

#include <QBuffer>
#include <QImage>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <limits>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage ai_mask_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] EditAiMaskExecutionResult execute_subject_mask(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const BackendSubjectMaskRequest request
) {
    EditAiMaskExecutionResult result;
    result.result.job_token = request.job_token;
    result.result.generation = request.generation;
    try {
        result.result = backend->executeSubjectMaskJob(photo_id, source_path, request);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditAiMaskApplyResult apply_subject_mask(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const BackendSubjectMaskApplyRequest request
) {
    EditAiMaskApplyResult result;
    try {
        result.state = backend->applySubjectMaskProposal(photo_id, source_path, request);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] QString candidate_preview_source(const BackendSubjectMaskResult& result) {
    if (result.preview_width == 0U || result.preview_height == 0U
        || result.preview_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || result.preview_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const auto expected_size = static_cast<std::uint64_t>(result.preview_width)
                               * static_cast<std::uint64_t>(result.preview_height);
    if (expected_size != static_cast<std::uint64_t>(result.preview_samples.size())) {
        return {};
    }

    QImage image{
        static_cast<int>(result.preview_width),
        static_cast<int>(result.preview_height),
        QImage::Format_RGBA8888,
    };
    if (image.isNull()) {
        return {};
    }
    for (std::uint32_t row = 0U; row < result.preview_height; ++row) {
        auto* const scan_line = image.scanLine(static_cast<int>(row));
        for (std::uint32_t column = 0U; column < result.preview_width; ++column) {
            const auto sample_index =
                static_cast<qsizetype>(row) * static_cast<qsizetype>(result.preview_width)
                + static_cast<qsizetype>(column);
            const auto rgba_offset = static_cast<std::size_t>(column) * 4U;
            scan_line[rgba_offset] = 0xffU;
            scan_line[rgba_offset + 1U] = 0xffU;
            scan_line[rgba_offset + 2U] = 0xffU;
            scan_line[rgba_offset + 3U] =
                static_cast<std::uint8_t>(result.preview_samples.at(sample_index));
        }
    }
    QByteArray png;
    QBuffer buffer{&png};
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return {};
    }
    return QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(png.toBase64()));
}

[[nodiscard]] QString person_thumbnail_source(const QByteArray& jpeg) {
    if (jpeg.isEmpty()) {
        return {};
    }
    return QStringLiteral("data:image/jpeg;base64,%1").arg(QString::fromLatin1(jpeg.toBase64()));
}

} // namespace

EditAiMaskController::EditAiMaskController(
    EditController& owner,
    std::shared_ptr<DesktopBackend> backend
) : owner_(owner), backend_(std::move(backend)) {
    QObject::connect(
        &execution_watcher_,
        &QFutureWatcher<EditAiMaskExecutionResult>::finished,
        &owner_,
        [this] { finishExecution(); }
    );
    QObject::connect(
        &apply_watcher_,
        &QFutureWatcher<EditAiMaskApplyResult>::finished,
        &owner_,
        [this] { finishApply(); }
    );
    pending_generation_retry_.setSingleShot(true);
    QObject::connect(&pending_generation_retry_, &QTimer::timeout, &owner_, [this] {
        tryStartPendingGeneration();
    });
    const auto schedule_pending_generation_retry = [this] {
        if (generation_pending_) {
            pending_generation_retry_.start(0);
        }
    };
    QObject::connect(
        &owner_,
        &EditController::autosavePendingChanged,
        &pending_generation_retry_,
        schedule_pending_generation_retry
    );
    QObject::connect(
        &owner_,
        &EditController::stateBusyChanged,
        &pending_generation_retry_,
        schedule_pending_generation_retry
    );
}

bool EditController::aiMaskPromptActive() const noexcept {
    return ai_mask_controller_ && ai_mask_controller_->active();
}

bool EditController::aiMaskBusy() const noexcept {
    return ai_mask_controller_ && ai_mask_controller_->busy();
}

bool EditController::aiMaskFaceRegionMode() const noexcept {
    return ai_mask_controller_ && ai_mask_controller_->faceRegionMode();
}

bool EditController::aiMaskSemanticMode() const noexcept {
    return ai_mask_controller_ && ai_mask_controller_->semanticMode();
}

QString EditController::aiMaskSemanticQuery() const {
    return ai_mask_controller_ ? ai_mask_controller_->semanticQuery() : QString{};
}

int EditController::aiMaskFaceRegion() const noexcept {
    return ai_mask_controller_ ? ai_mask_controller_->faceRegion() : 0;
}

QVariantList EditController::aiMaskPeople() const {
    return ai_mask_controller_ ? ai_mask_controller_->people() : QVariantList{};
}

int EditController::aiMaskSelectedPerson() const noexcept {
    return ai_mask_controller_ ? ai_mask_controller_->selectedPerson() : -1;
}

int EditController::aiMaskFaceRegionMask() const noexcept {
    return ai_mask_controller_ ? ai_mask_controller_->faceRegionMask() : 0;
}

bool EditController::aiMaskForegroundMode() const noexcept {
    return !ai_mask_controller_ || ai_mask_controller_->foregroundMode();
}

QVariantList EditController::aiMaskPromptPoints() const {
    return ai_mask_controller_ ? ai_mask_controller_->promptPoints() : QVariantList{};
}

bool EditController::aiMaskCanGenerate() const noexcept {
    return ai_mask_controller_ && ai_mask_controller_->canGenerate();
}

bool EditController::rawDenoiseExecutionAllowed() const noexcept {
    return ai_preferences_ == nullptr || ai_preferences_->rawDenoiseExecutionAllowed();
}

bool EditController::subjectMaskExecutionAllowed() const noexcept {
    return ai_preferences_ == nullptr || ai_preferences_->subjectMaskExecutionAllowed();
}

bool EditController::aiMaskHasCandidate() const noexcept {
    return ai_mask_controller_ && ai_mask_controller_->hasCandidate();
}

QString EditController::aiMaskCandidateSource() const {
    return ai_mask_controller_ ? ai_mask_controller_->candidateSource() : QString{};
}

bool EditController::beginAiMaskPrompt() {
    return ai_mask_controller_
           && ai_mask_controller_->beginPrompt(AiMaskSelectionKind::PromptedSubject);
}

bool EditController::beginAiFaceMaskPrompt() {
    return ai_mask_controller_ && ai_mask_controller_->beginPrompt(AiMaskSelectionKind::FaceRegion);
}

bool EditController::beginAiSemanticMask(const QString& query) {
    return ai_mask_controller_
           && ai_mask_controller_->beginSemantic(query, 4U, 30U, false);
}

void EditController::setAiMaskForegroundMode(const bool foreground) {
    if (ai_mask_controller_) {
        ai_mask_controller_->setForegroundMode(foreground);
    }
}

void EditController::setAiMaskFaceRegion(const int region) {
    if (ai_mask_controller_) {
        ai_mask_controller_->setFaceRegion(region);
    }
}

void EditController::setAiMaskSelectedPerson(const int person_index) {
    if (ai_mask_controller_) {
        ai_mask_controller_->setSelectedPerson(person_index);
    }
}

void EditController::toggleAiMaskFaceRegion(const int region, const bool selected) {
    if (ai_mask_controller_) {
        ai_mask_controller_->toggleFaceRegion(region, selected);
    }
}

void EditController::addAiMaskPromptPoint(
    const double normalized_x,
    const double normalized_y,
    const bool foreground
) {
    if (ai_mask_controller_) {
        ai_mask_controller_->appendPoint(normalized_x, normalized_y, foreground);
    }
}

void EditController::undoAiMaskPromptPoint() {
    if (ai_mask_controller_) {
        ai_mask_controller_->undoPoint();
    }
}

void EditController::clearAiMaskPromptPoints() {
    if (ai_mask_controller_) {
        ai_mask_controller_->clearPoints();
    }
}

void EditController::generateAiMask() {
    if (ai_mask_controller_) {
        ai_mask_controller_->generate();
    }
}

void EditController::applyAiMaskCandidate() {
    if (ai_mask_controller_) {
        ai_mask_controller_->applyCandidate();
    }
}

void EditController::cancelAiMaskPrompt() {
    if (ai_mask_controller_) {
        ai_mask_controller_->cancel();
    }
}

void EditController::applySubjectMaskState(
    BackendPhotoEditState state,
    const BackendGradeStack& before,
    const QString& target_grade_node_id
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
    base_commit_id_ = state.base_commit_id;
    durable_working_commit_id_ = state.base_commit_id;
    committed_grade_stack_ = state.grade_stack;
    setGradeStack(std::move(state.grade_stack), target_grade_node_id);
    history_.record(
        QStringLiteral("grade_node/%1/local_mask/ai").arg(target_grade_node_id).toStdString(),
        before,
        grade_stack_
    );
    if (could_undo != canUndo() || could_redo != canRedo()) {
        emit historyChanged();
    }
    working_revision_ = 0;
    persistence_state_.resetAutosaveSnapshot();
    versions_.replace(std::move(state.versions));
    setDirty(false);
    schedulePreview(0);
}

EditAiMaskController::~EditAiMaskController() {
    pending_generation_retry_.stop();
    QObject::disconnect(&execution_watcher_, nullptr, &owner_, nullptr);
    QObject::disconnect(&apply_watcher_, nullptr, &owner_, nullptr);
    const auto job_token = prompt_state_.reset_context();
    if (job_token) {
        try {
            backend_->cancelSubjectMaskJob(*job_token);
        } catch (...) {}
    }
    retireInputSession();
    retireCandidate();
    execution_watcher_.waitForFinished();
    if (execution_watcher_.future().isValid()) {
        const EditAiMaskExecutionResult result = execution_watcher_.result();
        if (result.result.terminal == BackendSubjectMaskTerminal::Staged) {
            discardProposal(result.result.proposal_token);
        }
    }
    apply_watcher_.waitForFinished();
}

bool EditAiMaskController::active() const noexcept {
    return active_;
}

bool EditAiMaskController::busy() const noexcept {
    return generation_pending_ || execution_watcher_.isRunning() || prompt_state_.busy()
           || apply_in_flight_;
}

bool EditAiMaskController::locksInteraction() const noexcept {
    return active_ || busy();
}

bool EditAiMaskController::foregroundMode() const noexcept {
    return foreground_mode_;
}

bool EditAiMaskController::faceRegionMode() const noexcept {
    return active_ && selection_kind_ == AiMaskSelectionKind::FaceRegion;
}

bool EditAiMaskController::semanticMode() const noexcept {
    return active_ && selection_kind_ == AiMaskSelectionKind::SemanticQuery;
}

QString EditAiMaskController::semanticQuery() const {
    return semanticMode() ? semantic_query_ : QString{};
}

int EditAiMaskController::faceRegion() const noexcept {
    return static_cast<int>(face_region_);
}

QVariantList EditAiMaskController::people() const {
    QVariantList result;
    result.reserve(people_.size());
    for (const auto& person : people_) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("index"), person.index},
                {QStringLiteral("confidence"), person.confidence},
                {QStringLiteral("thumbnailSource"), person_thumbnail_source(person.thumbnail_jpeg)},
                {QStringLiteral("regionsAnalyzed"), person.regions_analyzed},
                {QStringLiteral("availableRegionMask"), person.available_region_mask},
            }
        );
    }
    return result;
}

int EditAiMaskController::selectedPerson() const noexcept {
    return selected_person_;
}

int EditAiMaskController::faceRegionMask() const noexcept {
    return static_cast<int>(face_region_mask_);
}

bool EditAiMaskController::canGenerate() const noexcept {
    return owner_.subjectMaskExecutionAllowed() && active_ && !busy() && contextIsCurrent()
           && selectionReady();
}

bool EditAiMaskController::hasCandidate() const noexcept {
    return active_ && !busy() && contextIsCurrent() && candidate_proposal_token_ != 0
           && candidate_generation_ == prompt_state_.generation() && !candidate_source_.isEmpty();
}

QString EditAiMaskController::candidateSource() const {
    return hasCandidate() ? candidate_source_ : QString{};
}

QVariantList EditAiMaskController::promptPoints() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(prompt_state_.points().size()));
    for (const auto& point : prompt_state_.points()) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("x"), point.x},
                {QStringLiteral("y"), point.y},
                {
                    QStringLiteral("foreground"),
                    point.polarity == shadow::desktop::AiMaskPromptPolarity::Foreground,
                },
            }
        );
    }
    return result;
}

bool EditAiMaskController::beginPrompt(const AiMaskSelectionKind kind) {
    return beginSelection(kind, false);
}

bool EditAiMaskController::beginSemantic(
    const QString& query,
    const std::uint8_t maximum_regions,
    const std::uint8_t score_threshold_percent,
    const bool prefer_current_node
) {
    const QString normalized = query.simplified();
    if (normalized.isEmpty() || normalized.toUtf8().size() > 256 || maximum_regions < 1U
        || maximum_regions > 8U || score_threshold_percent < 1U
        || score_threshold_percent > 100U) {
        owner_.setStatusMessage(ai_mask_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Enter a semantic subject of at most 256 bytes"
        )));
        return false;
    }
    semantic_query_ = normalized;
    semantic_maximum_regions_ = maximum_regions;
    semantic_score_threshold_percent_ = score_threshold_percent;
    if (beginSelection(AiMaskSelectionKind::SemanticQuery, prefer_current_node)) {
        return true;
    }
    semantic_query_.clear();
    return false;
}

bool EditAiMaskController::beginSelection(
    const AiMaskSelectionKind kind,
    const bool prefer_current_node
) {
    if (active_) {
        return selection_kind_ == kind;
    }
    if (!owner_.subjectMaskExecutionAllowed()) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI subject selection is disabled in Settings")
        ));
        return false;
    }
    const auto* current_target = owner_.selectedGradeNode();
    const bool can_use_current = prefer_current_node && current_target != nullptr
                                 && current_target->enabled
                                 && current_target->local_mask_kind == 0U;
    if (!owner_.active_ || owner_.interactionLocked()
        || (!can_use_current && !owner_.canAddGradeNode())) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI selection needs room for a new Grade Node")
        ));
        return false;
    }

    std::uint64_t input_session_token = 0;
    try {
        input_session_token = backend_->beginSubjectMaskInputSession();
    } catch (const std::exception& error) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Could not start AI Mask · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return false;
    }

    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    const qsizetype previous_node_count = owner_.grade_stack_.grade_nodes.size();
    if (!can_use_current) {
        owner_.addGradeNode();
    }
    const auto* const target = owner_.selectedGradeNode();
    const bool target_count_valid = can_use_current
                                        ? owner_.grade_stack_.grade_nodes.size()
                                              == previous_node_count
                                        : owner_.grade_stack_.grade_nodes.size()
                                              == previous_node_count + 1;
    if (!target_count_valid || target == nullptr
        || !target->enabled || target->local_mask_kind != 0U) {
        try {
            backend_->finishSubjectMaskInputSession(input_session_token);
        } catch (...) {}
        return false;
    }
    owner_.finishActiveGesture();
    owner_.setRetouchPickerActive(false);
    owner_.setPointColorPickerActive(false);
    owner_.setWhiteBalancePickerActive(false);
    owner_.setCropToolActive(false);
    static_cast<void>(prompt_state_.reset_context());
    selection_kind_ = kind;
    face_region_ = BackendFaceRegion::Face;
    people_.clear();
    selected_person_ = -1;
    face_region_mask_ = 1U;
    people_discovery_complete_ = false;
    foreground_mode_ = true;
    context_ = CapturedContext{
        .photo_id = owner_.photo_id_,
        .source_path = owner_.source_path_,
        .target_grade_node_id = target->grade_node_id,
        .target_grade_node_index = static_cast<std::uint32_t>(owner_.selected_grade_node_index_),
        .grade_stack = owner_.grade_stack_,
        .photo_generation = owner_.photo_generation_,
        .input_session_token = input_session_token,
    };
    active_ = true;
    publishStateChange(previous_busy, previously_locked);
    owner_.setStatusMessage(ai_mask_message(
        kind == AiMaskSelectionKind::FaceRegion
            ? QT_TRANSLATE_NOOP("EditController", "People details · detecting people…")
        : kind == AiMaskSelectionKind::SemanticQuery
            ? QT_TRANSLATE_NOOP("EditController", "Semantic Mask · locating %1…")
            : QT_TRANSLATE_NOOP(
                  "EditController",
                  "AI Mask · click the subject to add an include point"
              ),
        kind == AiMaskSelectionKind::SemanticQuery
            ? std::initializer_list<LocalizedUiArgument>{semantic_query_}
            : std::initializer_list<LocalizedUiArgument>{}
    ));
    if (kind == AiMaskSelectionKind::FaceRegion || kind == AiMaskSelectionKind::SemanticQuery) {
        requestGeneration();
    }
    return true;
}

void EditAiMaskController::setForegroundMode(const bool foreground) {
    if (!active_ || faceRegionMode() || semanticMode() || busy()
        || foreground_mode_ == foreground) {
        return;
    }
    foreground_mode_ = foreground;
    emit owner_.aiMaskPromptChanged();
}

void EditAiMaskController::setFaceRegion(const int region) {
    if (!faceRegionMode() || busy() || region < static_cast<int>(BackendFaceRegion::Face)
        || region > static_cast<int>(BackendFaceRegion::Accessories)
        || region == static_cast<int>(face_region_)) {
        return;
    }
    face_region_ = static_cast<BackendFaceRegion>(region);
    face_region_mask_ = 1U << static_cast<std::uint32_t>(region);
    static_cast<void>(prompt_state_.invalidate_selection());
    retireCandidate();
    emit owner_.aiMaskPromptChanged();
    if (selected_person_ >= 0) {
        requestGeneration();
    }
}

void EditAiMaskController::setSelectedPerson(const int person_index) {
    if (!faceRegionMode() || busy() || person_index < 0 || person_index >= people_.size()
        || selected_person_ == person_index) {
        return;
    }
    selected_person_ = person_index;
    static_cast<void>(prompt_state_.invalidate_selection());
    retireCandidate();
    emit owner_.aiMaskPromptChanged();
    requestGeneration();
}

void EditAiMaskController::toggleFaceRegion(const int region, const bool selected) {
    if (!faceRegionMode() || busy() || region < static_cast<int>(BackendFaceRegion::Face)
        || region > static_cast<int>(BackendFaceRegion::Accessories)) {
        return;
    }
    const std::uint32_t bit = 1U << static_cast<std::uint32_t>(region);
    const std::uint32_t updated = selected ? face_region_mask_ | bit : face_region_mask_ & ~bit;
    if (updated == 0U || updated == face_region_mask_) {
        return;
    }
    face_region_mask_ = updated;
    face_region_ = static_cast<BackendFaceRegion>(region);
    static_cast<void>(prompt_state_.invalidate_selection());
    retireCandidate();
    emit owner_.aiMaskPromptChanged();
    requestGeneration();
}

void EditAiMaskController::appendPoint(const double x, const double y, const bool foreground) {
    if (!active_ || semanticMode()) {
        return;
    }
    if (faceRegionMode() && !prompt_state_.points().empty()) {
        static_cast<void>(prompt_state_.clear_points());
    }
    const auto mutation = prompt_state_.append_point({
        .x = x,
        .y = y,
        .polarity = faceRegionMode() || foreground
                        ? shadow::desktop::AiMaskPromptPolarity::Foreground
                        : shadow::desktop::AiMaskPromptPolarity::Background,
    });
    if (mutation == shadow::desktop::AiMaskPromptMutationResult::PointLimitReached) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI Mask accepts at most 16 prompt points")
        ));
        return;
    }
    if (mutation != shadow::desktop::AiMaskPromptMutationResult::Applied) {
        return;
    }
    retireCandidate();
    foreground_mode_ = faceRegionMode() || foreground;
    emit owner_.aiMaskPromptChanged();
    requestGeneration();
}

void EditAiMaskController::undoPoint() {
    if (prompt_state_.undo_point() == shadow::desktop::AiMaskPromptMutationResult::Applied) {
        retireCandidate();
        emit owner_.aiMaskPromptChanged();
        if (hasForegroundPoint()) {
            requestGeneration();
        } else {
            generation_pending_ = false;
            owner_.setStatusMessage(ai_mask_message(
                QT_TRANSLATE_NOOP("EditController", "AI Mask · click the subject to select it")
            ));
        }
    }
}

void EditAiMaskController::clearPoints() {
    if (prompt_state_.clear_points() == shadow::desktop::AiMaskPromptMutationResult::Applied) {
        retireCandidate();
        generation_pending_ = false;
        emit owner_.aiMaskPromptChanged();
        owner_.setStatusMessage(ai_mask_message(
            faceRegionMode()
                ? QT_TRANSLATE_NOOP(
                      "EditController",
                      "People details · choose a person and details"
                  )
                : QT_TRANSLATE_NOOP("EditController", "AI Mask · click the subject to select it")
        ));
    }
}

void EditAiMaskController::generate() {
    if (faceRegionMode() && people_discovery_complete_ && people_.isEmpty()) {
        people_discovery_complete_ = false;
        static_cast<void>(prompt_state_.invalidate_selection());
    }
    requestGeneration();
}

void EditAiMaskController::requestGeneration() {
    if (!active_ || apply_in_flight_ || !contextIsCurrent() || !selectionReady()) {
        return;
    }
    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    generation_pending_ = true;
    publishStateChange(previous_busy, previously_locked);
    tryStartPendingGeneration();
}

void EditAiMaskController::tryStartPendingGeneration() {
    if (!generation_pending_ || !active_ || apply_in_flight_ || execution_watcher_.isRunning()
        || prompt_state_.busy()) {
        return;
    }
    if (!contextIsCurrent() || !selectionReady()) {
        const bool previous_busy = owner_.busy();
        const bool previously_locked = owner_.interactionLocked();
        generation_pending_ = false;
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    if (!owner_.subjectMaskExecutionAllowed()) {
        const bool previous_busy = owner_.busy();
        const bool previously_locked = owner_.interactionLocked();
        generation_pending_ = false;
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI subject selection is disabled in Settings")
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    if (owner_.autosaveFailed()) {
        const bool previous_busy = owner_.busy();
        const bool previously_locked = owner_.interactionLocked();
        generation_pending_ = false;
        owner_.setStatusMessage(ai_mask_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI Mask cannot continue until the current adjustments are saved"
        )));
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    if (owner_.dirty_ || owner_.stateTaskRunning()) {
        if (owner_.dirty_ && !owner_.stateTaskRunning()) {
            owner_.persistence_state_.requestAutosave();
            owner_.startAutosave();
        }
        owner_.setStatusMessage(ai_mask_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI Mask is waiting for the current adjustments to finish saving"
        )));
        emit owner_.aiMaskPromptChanged();
        return;
    }
    startGeneration();
}

void EditAiMaskController::startGeneration() {
    if (!generation_pending_ || !active_ || execution_watcher_.isRunning() || prompt_state_.busy()
        || apply_in_flight_ || !contextIsCurrent() || !selectionReady()) {
        return;
    }
    std::uint64_t job_token = 0;
    try {
        job_token = backend_->beginSubjectMaskJob();
    } catch (const std::exception& error) {
        const bool previous_busy = owner_.busy();
        const bool previously_locked = owner_.interactionLocked();
        generation_pending_ = false;
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Could not start AI Mask · %1"),
            {QString::fromUtf8(error.what())}
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    const auto snapshot =
        prompt_state_.begin_request(job_token, faceRegionMode() || semanticMode());
    if (!snapshot) {
        try {
            backend_->cancelSubjectMaskJob(job_token);
        } catch (...) {}
        const bool previous_busy = owner_.busy();
        const bool previously_locked = owner_.interactionLocked();
        generation_pending_ = false;
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    retireCandidate();

    active_request_kind_ = semanticMode()
                               ? BackendSubjectMaskKind::SemanticQuery
                           : faceRegionMode()
                               ? people_discovery_complete_
                                     ? BackendSubjectMaskKind::PeopleRegions
                                     : BackendSubjectMaskKind::PeopleDiscovery
                               : BackendSubjectMaskKind::PromptedSubject;
    BackendSubjectMaskRequest request{
        .input_session_token = context_->input_session_token,
        .job_token = snapshot->job_token,
        .generation = snapshot->generation,
        .base_commit_id = owner_.base_commit_id_,
        .grade_stack = owner_.grade_stack_,
        .target_grade_node_index = context_->target_grade_node_index,
        .target_grade_node_id = context_->target_grade_node_id,
        .kind = active_request_kind_,
        .person_index = selected_person_ < 0 ? 0U : static_cast<std::uint32_t>(selected_person_),
        .face_region_mask = face_region_mask_,
        .semantic_query = semanticMode() ? semantic_query_ : QString{},
        .semantic_maximum_regions = semanticMode() ? semantic_maximum_regions_ : std::uint8_t{0},
        .semantic_score_threshold_percent =
            semanticMode() ? semantic_score_threshold_percent_ : std::uint8_t{0},
    };
    request.points.reserve(static_cast<qsizetype>(snapshot->points.size()));
    for (const auto& point : snapshot->points) {
        request.points.push_back({
            .x = point.x,
            .y = point.y,
            .foreground = point.polarity == shadow::desktop::AiMaskPromptPolarity::Foreground,
        });
    }

    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    generation_pending_ = false;
    execution_watcher_.setFuture(
        QtConcurrent::run(
            execute_subject_mask,
            backend_,
            context_->photo_id,
            context_->source_path,
            std::move(request)
        )
    );
    publishStateChange(previous_busy, previously_locked);
    owner_.setStatusMessage(ai_mask_message(
        semanticMode()
            ? QT_TRANSLATE_NOOP("EditController", "Semantic Mask is locating %1…")
        : faceRegionMode()
            ? active_request_kind_ == BackendSubjectMaskKind::PeopleDiscovery
                  ? QT_TRANSLATE_NOOP("EditController", "AI Mask is detecting people…")
                  : QT_TRANSLATE_NOOP("EditController", "AI Mask is identifying facial details…")
            : QT_TRANSLATE_NOOP("EditController", "AI Mask is identifying the subject…"),
        semanticMode() ? std::initializer_list<LocalizedUiArgument>{semantic_query_}
                       : std::initializer_list<LocalizedUiArgument>{}
    ));
}

void EditAiMaskController::applyCandidate() {
    if (!hasCandidate() || !context_) {
        return;
    }
    BackendSubjectMaskApplyRequest apply_request{
        .proposal_token = candidate_proposal_token_,
        .generation = candidate_generation_,
        .base_commit_id = owner_.base_commit_id_,
        .expected_working_commit_id = owner_.durable_working_commit_id_,
        .grade_stack = owner_.grade_stack_,
        .target_grade_node_index = context_->target_grade_node_index,
        .target_grade_node_id = context_->target_grade_node_id,
        .invert = false,
        .semantic_query = semanticMode() ? semantic_query_ : QString{},
        .semantic_maximum_regions = semanticMode() ? semantic_maximum_regions_ : std::uint8_t{0},
        .semantic_score_threshold_percent =
            semanticMode() ? semantic_score_threshold_percent_ : std::uint8_t{0},
    };
    candidate_proposal_token_ = 0;
    candidate_generation_ = 0;
    candidate_source_.clear();
    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    apply_in_flight_ = true;
    apply_watcher_.setFuture(
        QtConcurrent::run(
            apply_subject_mask,
            backend_,
            context_->photo_id,
            context_->source_path,
            std::move(apply_request)
        )
    );
    owner_.setStatusMessage(
        ai_mask_message(QT_TRANSLATE_NOOP("EditController", "Applying AI Mask…"))
    );
    publishStateChange(previous_busy, previously_locked);
}

void EditAiMaskController::cancel() {
    if (!active_ && !busy()) {
        return;
    }
    if (apply_in_flight_) {
        return;
    }
    resetContext();
    owner_.setStatusMessage(
        ai_mask_message(QT_TRANSLATE_NOOP("EditController", "AI Mask cancelled"))
    );
}

void EditAiMaskController::resetContext() {
    if (!active_ && !context_ && !busy()) {
        return;
    }
    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    const auto job_token = prompt_state_.reset_context();
    if (job_token) {
        try {
            backend_->cancelSubjectMaskJob(*job_token);
        } catch (...) {}
    }
    retireInputSession();
    pending_generation_retry_.stop();
    generation_pending_ = false;
    retireCandidate();
    active_ = false;
    foreground_mode_ = true;
    selection_kind_ = AiMaskSelectionKind::PromptedSubject;
    semantic_query_.clear();
    semantic_maximum_regions_ = 4U;
    semantic_score_threshold_percent_ = 30U;
    people_.clear();
    selected_person_ = -1;
    face_region_mask_ = 1U;
    people_discovery_complete_ = false;
    context_.reset();
    publishStateChange(previous_busy, previously_locked);
}

void EditAiMaskController::finishExecution() {
    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    const EditAiMaskExecutionResult task = execution_watcher_.result();
    const auto completion =
        prompt_state_.complete_request(task.result.job_token, task.result.generation);
    if (!task.error.isEmpty()) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI Mask failed · %1"),
            {task.error}
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    if (completion != shadow::desktop::AiMaskPromptCompletion::Current || !contextIsCurrent()) {
        if (task.result.terminal == BackendSubjectMaskTerminal::Staged) {
            discardProposal(task.result.proposal_token);
        }
        publishStateChange(previous_busy, previously_locked);
        return;
    }

    switch (task.result.terminal) {
    case BackendSubjectMaskTerminal::Cancelled:
        owner_.setStatusMessage(
            ai_mask_message(QT_TRANSLATE_NOOP("EditController", "AI Mask cancelled"))
        );
        publishStateChange(previous_busy, previously_locked);
        return;
    case BackendSubjectMaskTerminal::Unavailable:
        owner_.setStatusMessage(ai_mask_message(
            semanticMode() ? QT_TRANSLATE_NOOP(
                                  "EditController",
                                  "Semantic Mask is unavailable or found no match · check Infer Runtime · %1"
                              )
            : faceRegionMode() ? QT_TRANSLATE_NOOP(
                                   "EditController",
                                   "Local face parsing is unavailable · check Infer Runtime · %1"
                               )
                             : QT_TRANSLATE_NOOP(
                                   "EditController",
                                   "Local SAM 2.1 is unavailable · check the model directory · %1"
                               ),
            {task.result.detail}
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    case BackendSubjectMaskTerminal::Failed:
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI Mask failed · %1"),
            {task.result.detail}
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    case BackendSubjectMaskTerminal::PeopleReady:
        people_ = task.result.people;
        people_discovery_complete_ = true;
        retireCandidate();
        if (people_.isEmpty()) {
            selected_person_ = -1;
            owner_.setStatusMessage(ai_mask_message(
                QT_TRANSLATE_NOOP("EditController", "People details · no people detected")
            ));
            publishStateChange(previous_busy, previously_locked);
            return;
        }
        if (selected_person_ < 0 || selected_person_ >= people_.size()) {
            selected_person_ = 0;
        }
        static_cast<void>(prompt_state_.invalidate_selection());
        if (!task.result.detail.isEmpty()) {
            owner_.setStatusMessage(ai_mask_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Selected details are not visible · choose another region"
            )));
            publishStateChange(previous_busy, previously_locked);
            return;
        }
        publishStateChange(previous_busy, previously_locked);
        requestGeneration();
        return;
    case BackendSubjectMaskTerminal::Staged:
        break;
    }
    if (task.result.proposal_token == 0) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI Mask returned no applicable proposal")
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    }

    const QString candidate_source = candidate_preview_source(task.result);
    if (candidate_source.isEmpty()) {
        discardProposal(task.result.proposal_token);
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "AI Mask returned an invalid candidate preview")
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    retireCandidate();
    candidate_proposal_token_ = task.result.proposal_token;
    candidate_generation_ = task.result.generation;
    candidate_source_ = candidate_source;
    if (!task.result.people.isEmpty()) {
        people_ = task.result.people;
    }
    owner_.setStatusMessage(ai_mask_message(
        semanticMode() ? QT_TRANSLATE_NOOP(
                             "EditController",
                             "Semantic candidate ready · confirm or cancel"
                         )
        : faceRegionMode() ? QT_TRANSLATE_NOOP(
                               "EditController",
                               "Facial detail candidate ready · choose another region or apply"
                           )
                         : QT_TRANSLATE_NOOP(
                               "EditController",
                               "AI Mask candidate ready · add points to refine or apply"
                           )
    ));
    publishStateChange(previous_busy, previously_locked);
}

void EditAiMaskController::finishApply() {
    const bool previous_busy = owner_.busy();
    const bool previously_locked = owner_.interactionLocked();
    EditAiMaskApplyResult task = apply_watcher_.result();
    apply_in_flight_ = false;
    if (!task.error.isEmpty()) {
        owner_.setStatusMessage(ai_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Could not apply AI Mask · %1"),
            {task.error}
        ));
        publishStateChange(previous_busy, previously_locked);
        return;
    }
    if (!contextIsCurrent()) {
        active_ = false;
        static_cast<void>(prompt_state_.reset_context());
        retireInputSession();
        context_.reset();
        publishStateChange(previous_busy, previously_locked);
        return;
    }

    const BackendGradeStack before = owner_.grade_stack_;
    const QString target_grade_node_id = context_->target_grade_node_id;
    const bool applied_semantic = semanticMode();
    const QString applied_semantic_query = semantic_query_;
    active_ = false;
    foreground_mode_ = true;
    selection_kind_ = AiMaskSelectionKind::PromptedSubject;
    semantic_query_.clear();
    semantic_maximum_regions_ = 4U;
    semantic_score_threshold_percent_ = 30U;
    people_.clear();
    selected_person_ = -1;
    face_region_mask_ = 1U;
    people_discovery_complete_ = false;
    static_cast<void>(prompt_state_.reset_context());
    retireInputSession();
    context_.reset();
    owner_.applySubjectMaskState(std::move(task.state), before, target_grade_node_id);
    owner_.setStatusMessage(ai_mask_message(
        applied_semantic
            ? QT_TRANSLATE_NOOP(
                  "EditController",
                  "Semantic Mask for %1 applied · copying reruns it on the target photo"
              )
            : QT_TRANSLATE_NOOP("EditController", "AI Mask applied · Undo is available"),
        applied_semantic
            ? std::initializer_list<LocalizedUiArgument>{applied_semantic_query}
            : std::initializer_list<LocalizedUiArgument>{}
    ));
    publishStateChange(previous_busy, previously_locked);
}

void EditAiMaskController::publishStateChange(
    const bool previous_busy,
    const bool previously_locked
) {
    static_cast<void>(previous_busy);
    static_cast<void>(previously_locked);
    emit owner_.aiMaskPromptChanged();
    emit owner_.stateBusyChanged();
    emit owner_.gradeNodeActionsChanged();
    emit owner_.busyChanged();
}

bool EditAiMaskController::contextIsCurrent() const noexcept {
    if (!active_ || !context_ || owner_.photo_generation_ != context_->photo_generation
        || owner_.photo_id_ != context_->photo_id || owner_.source_path_ != context_->source_path
        || owner_.selected_grade_node_index_ != static_cast<int>(context_->target_grade_node_index)
        || owner_.grade_stack_ != context_->grade_stack) {
        return false;
    }
    const auto* const target = owner_.selectedGradeNode();
    return target != nullptr && target->grade_node_id == context_->target_grade_node_id
           && target->local_mask_kind == 0U;
}

bool EditAiMaskController::hasForegroundPoint() const noexcept {
    return std::ranges::any_of(
        prompt_state_.points(),
        [](const shadow::desktop::AiMaskPromptPoint& point) {
            return point.polarity == shadow::desktop::AiMaskPromptPolarity::Foreground;
        }
    );
}

bool EditAiMaskController::selectionReady() const noexcept {
    if (semanticMode()) {
        return !semantic_query_.isEmpty();
    }
    if (!faceRegionMode()) {
        return hasForegroundPoint();
    }
    return !people_discovery_complete_
           || (selected_person_ >= 0 && selected_person_ < people_.size()
               && face_region_mask_ != 0U);
}

void EditAiMaskController::retireInputSession() noexcept {
    if (!context_ || context_->input_session_token == 0) {
        return;
    }
    const std::uint64_t input_session_token = context_->input_session_token;
    context_->input_session_token = 0;
    try {
        backend_->finishSubjectMaskInputSession(input_session_token);
    } catch (...) {}
}

void EditAiMaskController::retireCandidate() noexcept {
    const std::uint64_t proposal_token = candidate_proposal_token_;
    candidate_proposal_token_ = 0;
    candidate_generation_ = 0;
    candidate_source_.clear();
    discardProposal(proposal_token);
}

void EditAiMaskController::discardProposal(const std::uint64_t proposal_token) const noexcept {
    if (proposal_token == 0) {
        return;
    }
    try {
        backend_->discardSubjectMaskProposal(proposal_token);
    } catch (...) {}
}
