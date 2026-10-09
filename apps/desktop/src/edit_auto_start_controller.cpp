#include "edit_auto_start_controller.hpp"
#include "ai_preferences.hpp"
#include "edit_controller.hpp"
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
namespace {
QString imageSource(const QByteArray& bytes) {
    return bytes.isEmpty() ? QString{}
                           : QStringLiteral("data:image/jpeg;base64,%1")
                                 .arg(QString::fromLatin1(bytes.toBase64()));
}
} // namespace
EditAutoStartController::EditAutoStartController(
    EditController& owner,
    std::shared_ptr<DesktopBackend> backend
) : QObject(&owner), owner_(owner), backend_(std::move(backend)) {
    preview_timer_.setSingleShot(true);
    preview_timer_.setInterval(90);
    automatic_timer_.setSingleShot(true);
    automatic_timer_.setInterval(750);
    connect(&preview_timer_, &QTimer::timeout, this, &EditAutoStartController::startPreview);
    connect(&automatic_timer_, &QTimer::timeout, this, &EditAutoStartController::maybeAutomatic);
    connect(
        &analysis_,
        &QFutureWatcher<AutoStartProposal>::finished,
        this,
        &EditAutoStartController::finishAnalysis
    );
    connect(
        &preview_,
        &QFutureWatcher<AutoStartPreviewResult>::finished,
        this,
        &EditAutoStartController::finishPreview
    );
    connect(
        &apply_,
        &QFutureWatcher<AutoStartApplyResult>::finished,
        this,
        &EditAutoStartController::finishApply
    );
    connect(&owner_, &EditController::sourceIdentityChanged, this, [this] {
        cancel();
        automatic_timer_.start();
    });
    const auto edited = [this] {
        if (active_ && !applying_ && !current())
            cancel();
    };
    connect(&owner_, &EditController::photoVariantsChanged, this, edited);
    connect(&owner_, &EditController::parametersChanged, this, edited);
    connect(&owner_, &EditController::foundationChanged, this, edited);
    connect(&owner_, &EditController::gradeNodesChanged, this, edited);
    connect(&owner_, &EditController::previewGeometryChanged, this, edited);
    const auto resume = [this] {
        if (pending_)
            QTimer::singleShot(0, this, &EditAutoStartController::startPending);
        emit changed();
    };
    connect(&owner_, &EditController::autosavePendingChanged, this, resume);
    connect(&owner_, &EditController::stateBusyChanged, this, resume);
    connect(&owner_, &EditController::previewSourceChanged, this, [this] {
        automatic_timer_.start();
    });
    connect(&owner_, &EditController::busyChanged, this, [this] {
        if (automaticEnabled() && !active_)
            automatic_timer_.start();
    });
    if (owner_.ai_preferences_) {
        connect(owner_.ai_preferences_, &AiPreferences::autoStartEnabledChanged, this, [this] {
            emit changed();
            automatic_timer_.start();
        });
        const auto permissionChanged = [this] {
            if (active_)
                cancel();
        };
        connect(
            owner_.ai_preferences_,
            &AiPreferences::imageUnderstandingExecutionAllowedChanged,
            this,
            permissionChanged
        );
        connect(
            owner_.ai_preferences_,
            &AiPreferences::subjectMaskExecutionAllowedChanged,
            this,
            permissionChanged
        );
    }
}
EditAutoStartController::~EditAutoStartController() {
    cancel();
    analysis_.waitForFinished();
    preview_.waitForFinished();
    apply_.waitForFinished();
    if (analysis_.isFinished() && analysis_.future().resultCount() > 0) {
        auto remaining = analysis_.result();
        discard(remaining);
    }
    discard(proposal_);
}
bool EditAutoStartController::current() const {
    return active_ && photo_ == owner_.photo_id_ && source_ == owner_.source_path_
           && photo_generation_ == owner_.photo_generation_
           && variant_ == owner_.active_variant_id_ && before_ == owner_.grade_stack_;
}
bool EditAutoStartController::canApply() const {
    return current() && ready() && !pending_ && !applying_ && !preview_.isRunning()
           && !preview_timer_.isActive() && !owner_.interactionLocked() && !owner_.dirty_
           && !owner_.stateTaskRunning() && presented_revision_ == preview_revision_
           && !preview_source_.isEmpty() && candidate() != before_;
}
BackendGradeStack EditAutoStartController::candidate() const {
    return autoStartStack(before_, tone_node_, proposal_, strength_, white_balance_, tone_, skin_);
}
QString EditAutoStartController::originalSource() const {
    return original_source_;
}
QString EditAutoStartController::summary() const {
    QStringList lines;
    if (proposal_.tone.preserveLight)
        lines << tr("Preserving the scene lighting; no global correction suggested.");
    else {
        if (whiteBalanceAvailable())
            lines << tr("White balance: a small correction around the current white point.");
        else
            lines << tr("White balance: no reliable RAW neutral estimate; kept unchanged.");
        if (toneAvailable())
            lines << tr("Tone: %1 EV, gentle highlight and shadow balance.")
                         .arg(proposal_.tone.basic.exposure_stops, 0, 'f', 2);
        else
            lines << tr("Tone: already balanced, or insufficient evidence to change it.");
    }
    if (skinAvailable())
        lines << tr(
            "Skin: subtle hue uniformity for %n person(s), preserving each person's colour.",
            nullptr,
            int(proposal_.skinNodes.size())
        );
    else
        lines
            << (proposal_.skinChecked ? tr("Skin: no confident correction suggested.")
                                      : tr("Skin: local selection was unavailable or skipped."));
    return lines.join(QLatin1Char('\n'));
}
bool EditAutoStartController::automaticEnabled() const {
    return owner_.ai_preferences_ && owner_.ai_preferences_->autoStartEnabled();
}
void EditAutoStartController::setAutomaticEnabled(bool v) {
    if (owner_.ai_preferences_)
        owner_.ai_preferences_->setAutoStartEnabled(v);
}
void EditAutoStartController::setStrength(double v) {
    if (!std::isfinite(v) || applying_)
        return;
    v = std::clamp(v, 0., 1.);
    if (v == strength_)
        return;
    strength_ = v;
    schedulePreview();
}
void EditAutoStartController::setWhiteBalanceEnabled(bool v) {
    if (!applying_ && white_balance_ != v) {
        white_balance_ = v;
        schedulePreview();
    }
}
void EditAutoStartController::setToneEnabled(bool v) {
    if (!applying_ && tone_ != v) {
        tone_ = v;
        schedulePreview();
    }
}
void EditAutoStartController::setSkinEnabled(bool v) {
    if (!applying_ && skin_ != v) {
        skin_ = v;
        schedulePreview();
    }
}
void EditAutoStartController::notify() {
    emit changed();
    emit owner_.busyChanged();
    emit owner_.stateBusyChanged();
    emit owner_.gradeNodeActionsChanged();
}
void EditAutoStartController::analyze() {
    if (active_ || busy() || !owner_.active_ || owner_.interactionLocked())
        return;
    if (!owner_.canAddGradeNode()) {
        status_ = tr("Automatic adjustment needs room for an adjustment node.");
        emit changed();
        return;
    }
    owner_.finishActiveGesture();
    try {
        tone_node_ = backend_->newBasicGradeNode(tr("Auto · tone"));
    } catch (const std::exception& e) {
        status_ = tr("Could not prepare suggestions · %1").arg(QString::fromUtf8(e.what()));
        emit changed();
        return;
    }
    automatically_attempted_.insert(owner_.photo_id_);
    before_ = owner_.grade_stack_;
    photo_ = owner_.photo_id_;
    source_ = owner_.source_path_;
    photo_generation_ = owner_.photo_generation_;
    variant_ = owner_.active_variant_id_;
    ++generation_;
    active_ = pending_ = true;
    strength_ = 1;
    white_balance_ = tone_ = skin_ = true;
    proposal_ = {};
    original_source_.clear();
    preview_source_.clear();
    presented_revision_ = 0;
    status_ = tr("Preparing automatic adjustments…");
    notify();
    startPending();
}
void EditAutoStartController::startPending() {
    if (!pending_ || analysis_.isRunning() || applying_)
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
    pending_ = false;
    base_ = owner_.base_commit_id_;
    cancellation_ = std::make_shared<AutoStartCancellation>();
    const AutoStartAnalysisInput input{
        photo_,
        source_,
        base_,
        before_,
        tone_node_,
        generation_,
        !owner_.ai_preferences_ || owner_.ai_preferences_->imageUnderstandingExecutionAllowed(),
        owner_.subjectMaskExecutionAllowed()
            && !(before_.liquify_enabled && !before_.liquify_strokes.isEmpty())
    };
    status_ = tr("Measuring tone and white balance; local AI will refine the suggestion…");
    const QPointer<EditAutoStartController> guard(this);
    analysis_.setFuture(
        QtConcurrent::run([backend = backend_, input, control = cancellation_, guard] {
            return analyzeAutoStart(
                backend,
                input,
                control,
                [guard, generation = input.generation](const AutoStartProposal& fast) {
                    if (!guard)
                        return;
                    QMetaObject::invokeMethod(
                        guard,
                        [guard, generation, fast] {
                            if (!guard || generation != guard->generation_ || !guard->current())
                                return;
                            guard->proposal_ = fast;
                            guard->original_source_ = imageSource(fast.original);
                            guard->schedulePreview();
                            guard->status_ = tr(
                                "Basic preview ready. Local AI is checking scene lighting and skin…"
                            );
                            guard->notify();
                        },
                        Qt::QueuedConnection
                    );
                }
            );
        })
    );
    notify();
}
void EditAutoStartController::finishAnalysis() {
    auto result = analysis_.result();
    if (!current() || !cancellation_ || cancellation_->cancelled.load()) {
        discard(result);
        notify();
        return;
    }
    discard(proposal_);
    proposal_ = std::move(result);
    original_source_ = imageSource(proposal_.original);
    if (proposal_.original.isEmpty()) {
        status_ = tr("Could not prepare suggestions · %1").arg(proposal_.error);
        notify();
        return;
    }
    status_ =
        proposal_.sceneChecked
            ? tr("Local AI suggestion ready · %1. Review before applying.").arg(proposal_.model)
            : tr("Measured suggestion ready. Scene AI is unavailable or disabled; review the "
                 "lighting.");
    schedulePreview();
    notify();
}
void EditAutoStartController::schedulePreview() {
    if (!ready() || applying_) {
        emit changed();
        return;
    }
    ++preview_revision_;
    if (render_token_)
        (void)backend_->cancelEditPreviewRequest(render_token_);
    preview_timer_.start();
    emit changed();
}
void EditAutoStartController::startPreview() {
    if (!current() || !ready() || applying_)
        return;
    if (preview_.isRunning())
        return; // finishPreview dispatches the latest revision.
    const auto revision = preview_revision_;
    render_token_ = backend_->beginEditPreviewRequest();
    const auto masks = skin_ && strength_ > 0 ? proposal_.masks : QVector<BackendAutoStartMask>{};
    preview_.setFuture(
        QtConcurrent::run([backend = backend_,
                           photo = photo_,
                           source = source_,
                           base = base_,
                           stack = candidate(),
                           masks,
                           token = render_token_,
                           revision] {
            AutoStartPreviewResult result;
            result.revision = revision;
            try {
                result.preview =
                    backend->renderAutoStartPreview(photo, source, base, stack, token, masks);
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        })
    );
    notify();
}
void EditAutoStartController::finishPreview() {
    const auto result = preview_.result();
    render_token_ = 0;
    if (!current()) {
        notify();
        return;
    }
    if (result.revision != preview_revision_) {
        startPreview();
        return;
    }
    if (!result.error.isEmpty()) {
        preview_source_.clear();
        status_ = tr("Could not preview suggestions · %1").arg(result.error);
    } else if (result.preview.terminal == EditPreviewTerminal::Completed) {
        preview_source_ = imageSource(result.preview.bytes);
        presented_revision_ = result.revision;
    }
    notify();
}
void EditAutoStartController::apply() {
    if (!canApply())
        return;
    // The measured preview is useful before optional models finish. Capture it
    // once, cancel the remaining analysis, and reject its late queued callbacks.
    if (cancellation_)
        cancellation_->cancel(*backend_);
    ++generation_;
    applying_ = true;
    const auto masks = skin_ && strength_ > 0 ? proposal_.masks : QVector<BackendAutoStartMask>{};
    status_ = tr("Applying automatic adjustments…");
    apply_.setFuture(
        QtConcurrent::run([backend = backend_,
                           photo = photo_,
                           source = source_,
                           base = owner_.base_commit_id_,
                           expected = owner_.durable_working_commit_id_,
                           variant = variant_,
                           stack = candidate(),
                           masks] {
            AutoStartApplyResult result;
            try {
                result.state =
                    backend->applyAutoStart(photo, source, base, expected, variant, stack, masks);
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        })
    );
    notify();
}
void EditAutoStartController::finishApply() {
    auto result = apply_.result();
    const bool valid = current();
    applying_ = false;
    if (!result.error.isEmpty()) {
        // Promotion is move-only: a failed apply cannot reuse partially consumed masks.
        discard(proposal_);
        active_ = false;
        preview_source_.clear();
        status_ = tr("Could not apply suggestions · %1. Analyze again to retry.").arg(result.error);
        notify();
        return;
    }
    active_ = false;
    discard(proposal_);
    preview_source_.clear();
    if (valid) {
        QString target = tone_node_.grade_node_id;
        if (!result.state.grade_stack.grade_nodes.isEmpty())
            target = result.state.grade_stack.grade_nodes.last().grade_node_id;
        owner_.selected_recipe_node_kind_ = QStringLiteral("grade");
        owner_.applySubjectMaskState(
            std::move(result.state),
            before_,
            target,
            QStringLiteral("auto_start")
        );
        status_ =
            tr("Automatic adjustments applied. Every adjustment remains editable; undo once to "
               "restore.");
        emit applied();
    }
    notify();
}
void EditAutoStartController::discard(AutoStartProposal& proposal) {
    for (const auto& mask : proposal.masks) {
        try {
            backend_->discardSubjectMaskProposal(mask.proposal_token);
        } catch (...) {}
    }
    proposal.masks.clear();
}
void EditAutoStartController::cancel() {
    if (applying_)
        return;
    const bool wasActive = active_;
    active_ = pending_ = false;
    ++generation_;
    ++preview_revision_;
    preview_timer_.stop();
    if (cancellation_)
        cancellation_->cancel(*backend_);
    if (render_token_)
        (void)backend_->cancelEditPreviewRequest(render_token_);
    // Render workers own immutable candidate bytes after resolving the plan. A
    // cancellation racing resolution is safe and cannot publish a stale frame.
    discard(proposal_);
    proposal_ = {};
    original_source_.clear();
    preview_source_.clear();
    if (wasActive)
        status_ = tr("Cancelled. The photo and edit history are unchanged.");
    notify();
}
void EditAutoStartController::maybeAutomatic() {
    if (!automaticEnabled() || active_ || busy() || !owner_.active_ || owner_.busy()
        || owner_.dirty_ || owner_.stateBusy() || !owner_.base_commit_id_.isEmpty()
        || owner_.canUndo() || owner_.previewSource().isEmpty()
        || automatically_attempted_.contains(owner_.photo_id_))
        return;
    automatically_attempted_.insert(owner_.photo_id_);
    analyze();
}
