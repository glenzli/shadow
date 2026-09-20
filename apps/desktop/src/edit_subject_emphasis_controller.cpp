#include "edit_subject_emphasis_controller.hpp"
#include "ai_preferences.hpp"
#include "edit_controller.hpp"
#include <QBuffer>
#include <QImage>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace {
QString maskSource(const BackendSubjectMaskResult& result) {
    if (result.preview_width != 256 || result.preview_height != 256
        || result.preview_samples.size() != 256 * 256)
        return {};
    QImage image(256, 256, QImage::Format_RGBA8888);
    for (int y = 0; y < 256; ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < 256; ++x) {
            row[4 * x] = 80;
            row[4 * x + 1] = 180;
            row[4 * x + 2] = 255;
            row[4 * x + 3] = static_cast<unsigned char>(result.preview_samples.at(y * 256 + x)) / 2;
        }
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(bytes.toBase64()));
}
} // namespace

EditSubjectEmphasisController::EditSubjectEmphasisController(
    EditController& owner,
    std::shared_ptr<DesktopBackend> backend
) : QObject(&owner), owner_(owner), backend_(std::move(backend)) {
    connect(
        &worker_,
        &QFutureWatcher<SubjectEmphasisTaskResult>::finished,
        this,
        &EditSubjectEmphasisController::finish
    );
    connect(
        &apply_worker_,
        &QFutureWatcher<SubjectEmphasisApplyResult>::finished,
        this,
        &EditSubjectEmphasisController::finishApply
    );
    connect(&owner_, &EditController::sourceIdentityChanged, this, [this] {
        if (active_)
            cancel();
        applied_id_.clear();
        status_.clear();
        emit changed();
    });
    connect(&owner_, &EditController::parametersChanged, this, [this] {
        if (active_ && !applying_ && !current())
            cancel();
        emit changed();
    });
    connect(
        &owner_,
        &EditController::selectedGradeNodeChanged,
        this,
        &EditSubjectEmphasisController::changed
    );
    const auto resume = [this] {
        if (pending_)
            QTimer::singleShot(0, this, &EditSubjectEmphasisController::startPending);
    };
    connect(&owner_, &EditController::autosavePendingChanged, this, resume);
    connect(&owner_, &EditController::stateBusyChanged, this, resume);
    if (owner_.ai_preferences_) {
        const auto check_permission = [this] {
            if (!owner_.subjectMaskExecutionAllowed()
                || !owner_.ai_preferences_->imageUnderstandingExecutionAllowed())
                cancel();
        };
        connect(
            owner_.ai_preferences_,
            &AiPreferences::subjectMaskExecutionAllowedChanged,
            this,
            check_permission
        );
        connect(
            owner_.ai_preferences_,
            &AiPreferences::imageUnderstandingExecutionAllowedChanged,
            this,
            check_permission
        );
    }
}

EditSubjectEmphasisController::~EditSubjectEmphasisController() {
    cancel();
    worker_.waitForFinished();
    apply_worker_.waitForFinished();
    retireProposal();
    retireInput();
}

bool EditSubjectEmphasisController::current() const noexcept {
    return active_ && photo_id_ == owner_.photo_id_ && source_path_ == owner_.source_path_
           && photo_generation_ == owner_.photo_generation_ && before_ == owner_.grade_stack_;
}
bool EditSubjectEmphasisController::canApply() const noexcept {
    return current() && !busy() && candidate_.proposal_token != 0
           && (candidate_.emphasis_reason == 1 || candidate_.emphasis_reason == 2) && strength_ > 0;
}
bool EditSubjectEmphasisController::applied() const noexcept {
    return !active_ && !applied_id_.isEmpty()
           && owner_.selectedRecipeNodeKind() == QStringLiteral("grade")
           && owner_.selectedGradeNodeId() == applied_id_;
}
double EditSubjectEmphasisController::strength() const noexcept {
    return applied() ? owner_.gradeNodeStrength() : strength_;
}
void EditSubjectEmphasisController::setStrength(double value) {
    if (!std::isfinite(value) || applying_)
        return;
    value = std::clamp(value, 0.0, 1.0);
    if (applied())
        owner_.setGradeNodeStrength(value);
    else
        strength_ = value;
    emit changed();
}
void EditSubjectEmphasisController::notify() {
    emit changed();
    emit owner_.stateBusyChanged();
    emit owner_.busyChanged();
    emit owner_.gradeNodeActionsChanged();
}

void EditSubjectEmphasisController::analyze() {
    if (active_ || busy() || !owner_.active_ || owner_.interactionLocked())
        return;
    if (!owner_.subjectMaskExecutionAllowed()
        || (owner_.ai_preferences_
            && !owner_.ai_preferences_->imageUnderstandingExecutionAllowed())) {
        status_ = tr("Enable local image understanding and subject selection in Settings first.");
        emit changed();
        return;
    }
    if (!owner_.canAddGradeNode()) {
        status_ = tr("Subject emphasis needs room for one adjustment node.");
        emit changed();
        return;
    }
    owner_.finishActiveGesture();
    owner_.setRetouchPickerActive(false);
    owner_.setPointColorPickerActive(false);
    owner_.setWhiteBalancePickerActive(false);
    owner_.setCropToolActive(false);
    try {
        auto node = backend_->newBasicGradeNode(tr("Subject emphasis"));
        input_ = backend_->beginSubjectMaskInputSession();
        before_ = owner_.grade_stack_;
        draft_ = before_;
        target_id_ = node.grade_node_id;
        draft_.grade_nodes.push_back(std::move(node));
    } catch (const std::exception& error) {
        status_ = tr("Could not start local analysis · %1").arg(QString::fromUtf8(error.what()));
        emit changed();
        return;
    }
    photo_id_ = owner_.photo_id_;
    source_path_ = owner_.source_path_;
    photo_generation_ = owner_.photo_generation_;
    ++generation_;
    active_ = true;
    pending_ = true;
    strength_ = 1.0;
    applied_id_.clear();
    queries_.clear();
    description_.clear();
    image_source_.clear();
    mask_source_.clear();
    query_.clear();
    kind_ = BackendSubjectMaskKind::SubjectAnalysis;
    status_ = tr("Preparing the current photo for local QwenVL…");
    notify();
    startPending();
}

void EditSubjectEmphasisController::selectSubject(const QString& query) {
    const QString cleaned = query.simplified();
    if (!current() || busy() || !analyzed() || cleaned.isEmpty() || cleaned.toUtf8().size() > 128)
        return;
    retireProposal();
    query_ = cleaned;
    ++generation_;
    kind_ = BackendSubjectMaskKind::SubjectEmphasis;
    pending_ = true;
    status_ = tr("Selecting “%1” locally…").arg(query_);
    notify();
    startPending();
}

void EditSubjectEmphasisController::startPending() {
    if (!pending_ || worker_.isRunning() || applying_)
        return;
    if (!current()) {
        cancel();
        return;
    }
    if (owner_.autosaveFailed()) {
        pending_ = false;
        status_ = tr("Save the current adjustments before analyzing.");
        notify();
        return;
    }
    if (owner_.dirty_ || owner_.stateTaskRunning()) {
        if (owner_.dirty_ && !owner_.stateTaskRunning()) {
            owner_.persistence_state_.requestAutosave();
            owner_.startAutosave();
        }
        return;
    }
    try {
        job_ = backend_->beginSubjectMaskJob();
    } catch (const std::exception& error) {
        pending_ = false;
        status_ = tr("Local analysis failed · %1").arg(QString::fromUtf8(error.what()));
        notify();
        return;
    }
    BackendSubjectMaskRequest request{
        .input_session_token = input_,
        .job_token = job_,
        .generation = generation_,
        .base_commit_id = owner_.base_commit_id_,
        .grade_stack = draft_,
        .target_grade_node_index = static_cast<std::uint32_t>(draft_.grade_nodes.size() - 1),
        .target_grade_node_id = target_id_,
        .kind = kind_,
        .semantic_query = query_,
        .semantic_maximum_regions = 1,
        .semantic_score_threshold_percent = 30,
    };
    pending_ = false;
    status_ = kind_ == BackendSubjectMaskKind::SubjectAnalysis
                  ? tr("QwenVL is analyzing locally. The first run may take a minute…")
                  : tr("Selecting “%1” locally…").arg(query_);
    worker_.setFuture(
        QtConcurrent::run([backend = backend_,
                           photo = photo_id_,
                           source = source_path_,
                           request = std::move(request)] {
            SubjectEmphasisTaskResult task;
            task.result.job_token = request.job_token;
            task.result.generation = request.generation;
            try {
                task.result = backend->executeSubjectMaskJob(photo, source, request);
            } catch (const std::exception& error) {
                task.error = QString::fromUtf8(error.what());
            }
            return task;
        })
    );
    notify();
}

void EditSubjectEmphasisController::finish() {
    auto task = worker_.result();
    job_ = 0;
    if (!current() || task.result.generation != generation_) {
        if (task.result.proposal_token) {
            try {
                backend_->discardSubjectMaskProposal(task.result.proposal_token);
            } catch (...) {}
        }
        notify();
        return;
    }
    if (!task.error.isEmpty()) {
        status_ = tr("Local analysis failed · %1").arg(task.error);
        notify();
        return;
    }
    if (task.result.terminal == BackendSubjectMaskTerminal::AnalysisReady) {
        description_ = task.result.description;
        queries_ = task.result.subject_queries;
        image_source_ = QStringLiteral("data:image/jpeg;base64,%1")
                            .arg(QString::fromLatin1(task.result.analysis_preview_jpeg.toBase64()));
        status_ = tr("%1 · choose the intended subject, or enter an English object name.")
                      .arg(task.result.analysis_model);
    } else if (task.result.terminal == BackendSubjectMaskTerminal::Staged) {
        candidate_ = std::move(task.result);
        mask_source_ = ::maskSource(candidate_);
        if (mask_source_.isEmpty()) {
            retireProposal();
            status_ = tr("The selection preview is invalid. Try another subject.");
        } else if (candidate_.emphasis_reason == 1)
            status_ =
                tr("Suggested: gently lift the subject (+0.18 EV). Check the blue selection before "
                   "applying.");
        else if (candidate_.emphasis_reason == 2)
            status_ =
                tr("Suggested: gently restrain the background (−0.12 EV), preserving subject "
                   "highlights.");
        else if (candidate_.emphasis_reason == 4)
            status_ = tr(
                "The selection is too broad, too small, or uncertain. Try a more specific subject."
            );
        else
            status_ =
                tr("The subject is already distinct, or the lighting may be intentional. No "
                   "adjustment is suggested.");
    } else {
        status_ = tr("Local selection is unavailable · %1").arg(task.result.detail);
    }
    notify();
}

void EditSubjectEmphasisController::apply() {
    if (!canApply())
        return;
    auto stack = draft_;
    auto& node = stack.grade_nodes.last();
    node.label = candidate_.emphasis_background ? tr("Subject emphasis · background")
                                                : tr("Subject emphasis · subject");
    node.basic.exposure_stops = candidate_.emphasis_exposure;
    node.basic.saturation_factor = candidate_.emphasis_saturation;
    node.opacity = strength_;
    BackendSubjectMaskApplyRequest request{
        .proposal_token = candidate_.proposal_token,
        .generation = candidate_.generation,
        .base_commit_id = owner_.base_commit_id_,
        .expected_working_commit_id = owner_.durable_working_commit_id_,
        .grade_stack = std::move(stack),
        .target_grade_node_index = static_cast<std::uint32_t>(draft_.grade_nodes.size() - 1),
        .target_grade_node_id = target_id_,
        .target_mask_operation = 0,
        .invert = candidate_.emphasis_background,
        .semantic_query = query_,
        .semantic_maximum_regions = 1,
        .semantic_score_threshold_percent = 30,
    };
    candidate_.proposal_token = 0;
    applying_ = true;
    status_ = tr("Applying subject emphasis…");
    apply_worker_.setFuture(
        QtConcurrent::run([backend = backend_,
                           photo = photo_id_,
                           source = source_path_,
                           request = std::move(request)] {
            SubjectEmphasisApplyResult task;
            try {
                task.state = backend->applySubjectMaskProposal(photo, source, request);
            } catch (const std::exception& error) {
                task.error = QString::fromUtf8(error.what());
            }
            return task;
        })
    );
    notify();
}

void EditSubjectEmphasisController::finishApply() {
    auto task = apply_worker_.result();
    applying_ = false;
    if (!task.error.isEmpty()) {
        status_ = tr("Could not apply subject emphasis · %1").arg(task.error);
        notify();
        return;
    }
    const bool valid = current();
    active_ = false;
    retireInput();
    mask_source_.clear();
    if (valid) {
        const QString id = target_id_;
        owner_.selected_recipe_node_kind_ = QStringLiteral("grade");
        owner_.applySubjectMaskState(std::move(task.state), before_, id);
        applied_id_ = id;
        status_ =
            tr("Applied as an editable node. Adjust strength, compare before/after, or undo once.");
    }
    notify();
}
void EditSubjectEmphasisController::cancel() {
    if (applying_)
        return;
    if (!active_ && !pending_)
        return;
    active_ = false;
    pending_ = false;
    ++generation_;
    if (job_) {
        try {
            backend_->cancelSubjectMaskJob(job_);
        } catch (...) {}
    }
    retireProposal();
    retireInput();
    image_source_.clear();
    queries_.clear();
    description_.clear();
    status_ = tr("Cancelled. The photo and edit history are unchanged.");
    notify();
}
void EditSubjectEmphasisController::retireProposal() {
    if (candidate_.proposal_token) {
        try {
            backend_->discardSubjectMaskProposal(candidate_.proposal_token);
        } catch (...) {}
    }
    candidate_ = {};
    mask_source_.clear();
}
void EditSubjectEmphasisController::retireInput() {
    if (input_) {
        try {
            backend_->finishSubjectMaskInputSession(input_);
        } catch (...) {}
        input_ = 0;
    }
}
