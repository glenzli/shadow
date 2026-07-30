#include "edit_raw_foundation_controller.hpp"

#include "edit_controller.hpp"

#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <utility>

namespace {

constexpr int RAW_FOUNDATION_PROGRESS_POLL_MS = 80;

[[nodiscard]] LocalizedUiMessage raw_foundation_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] EditRawFoundationProbeTaskResult probe_raw_foundation(
    const std::shared_ptr<DesktopBackend>& backend,
    const std::uint64_t context_generation
) {
    EditRawFoundationProbeTaskResult result;
    result.context_generation = context_generation;
    try {
        result.status = backend->probeRawFoundationRuntime();
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] EditRawFoundationExecutionTaskResult execute_raw_foundation(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const std::uint64_t job_token,
    const std::uint64_t context_generation
) {
    EditRawFoundationExecutionTaskResult result{
        .photo_id = photo_id,
        .source_path = source_path,
        .job_token = job_token,
        .context_generation = context_generation,
    };
    try {
        result.status = backend->executeRawFoundationJob(job_token, photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] shadow::desktop::EditRawFoundationPhase
edit_phase(const BackendRawFoundationJobPhase phase) noexcept {
    using BackendPhase = BackendRawFoundationJobPhase;
    using EditPhase = shadow::desktop::EditRawFoundationPhase;
    switch (phase) {
    case BackendPhase::Queued:
        return EditPhase::Queued;
    case BackendPhase::Planning:
        return EditPhase::Planning;
    case BackendPhase::Running:
        return EditPhase::Running;
    case BackendPhase::Ready:
        return EditPhase::Ready;
    case BackendPhase::Unavailable:
        return EditPhase::Unavailable;
    case BackendPhase::Cancelled:
        return EditPhase::Cancelled;
    case BackendPhase::Failed:
        return EditPhase::Failed;
    }
    return EditPhase::Failed;
}

[[nodiscard]] QString edit_phase_code(const shadow::desktop::EditRawFoundationPhase phase) {
    using Phase = shadow::desktop::EditRawFoundationPhase;
    switch (phase) {
    case Phase::Off:
        return QStringLiteral("off");
    case Phase::Checking:
        return QStringLiteral("checking");
    case Phase::Available:
        return QStringLiteral("available");
    case Phase::Queued:
        return QStringLiteral("queued");
    case Phase::Planning:
        return QStringLiteral("planning");
    case Phase::Running:
        return QStringLiteral("running");
    case Phase::Ready:
        return QStringLiteral("ready");
    case Phase::Unavailable:
        return QStringLiteral("unavailable");
    case Phase::Cancelled:
        return QStringLiteral("cancelled");
    case Phase::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

} // namespace

EditRawFoundationController::EditRawFoundationController(
    EditController& owner,
    std::shared_ptr<DesktopBackend> backend
) :
    owner_(owner), backend_(std::move(backend)),
    status_message_(
        raw_foundation_message(QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise is off"))
    ) {
    progress_timer_.setInterval(RAW_FOUNDATION_PROGRESS_POLL_MS);
    progress_timer_.setSingleShot(false);
    QObject::connect(&progress_timer_, &QTimer::timeout, &owner_, [this] { pollJob(); });
    QObject::connect(
        &probe_watcher_,
        &QFutureWatcher<EditRawFoundationProbeTaskResult>::finished,
        &owner_,
        [this] { finishProbe(); }
    );
    QObject::connect(
        &execution_watcher_,
        &QFutureWatcher<EditRawFoundationExecutionTaskResult>::finished,
        &owner_,
        [this] { finishExecution(); }
    );
    source_identity_connection_ =
        QObject::connect(&owner_, &EditController::sourceIdentityChanged, &owner_, [this] {
            resetContext();
        });
    foundation_connection_ =
        QObject::connect(&owner_, &EditController::rawAiDenoiseRecipeChanged, &owner_, [this] {
            syncRecipeState();
        });
    state_busy_connection_ =
        QObject::connect(&owner_, &EditController::stateBusyChanged, &owner_, [this] {
            maybeApplyReady();
        });
}

EditRawFoundationController::~EditRawFoundationController() {
    QObject::disconnect(source_identity_connection_);
    QObject::disconnect(foundation_connection_);
    QObject::disconnect(state_busy_connection_);
    QObject::disconnect(&probe_watcher_, nullptr, &owner_, nullptr);
    QObject::disconnect(&execution_watcher_, nullptr, &owner_, nullptr);
    QObject::disconnect(&progress_timer_, nullptr, &owner_, nullptr);
    progress_timer_.stop();
    if (const auto token = state_.request_cancellation()) {
        try {
            backend_->cancelRawFoundationJob(*token);
        } catch (...) {}
    }
    probe_watcher_.waitForFinished();
    execution_watcher_.waitForFinished();
    if (execution_watcher_.future().isValid()) {
        retireTerminal(execution_watcher_.result().status);
    }
}

bool EditController::foundationAiDenoiseEnabled() const noexcept {
    return raw_foundation_controller_ && raw_foundation_controller_->enabled();
}

int EditController::foundationAiDenoiseAmount() const noexcept {
    return static_cast<int>(grade_stack_.raw_ai_denoise.amount_percent);
}

bool EditController::foundationAiDenoiseAvailable() const noexcept {
    return raw_foundation_controller_ && raw_foundation_controller_->available();
}

bool EditController::foundationAiDenoiseBusy() const noexcept {
    return raw_foundation_controller_ && raw_foundation_controller_->busy();
}

bool EditController::foundationAiDenoiseCanStart() const noexcept {
    return raw_foundation_controller_ && raw_foundation_controller_->canStart();
}

bool EditController::foundationAiDenoiseCanCancel() const noexcept {
    return raw_foundation_controller_ && raw_foundation_controller_->canCancel();
}

QString EditController::foundationAiDenoisePhase() const {
    return raw_foundation_controller_ ? raw_foundation_controller_->phase() : QStringLiteral("off");
}

double EditController::foundationAiDenoiseProgress() const noexcept {
    return raw_foundation_controller_ ? raw_foundation_controller_->progress() : 0.0;
}

QString EditController::foundationAiDenoiseStatusText() const {
    return raw_foundation_controller_ ? raw_foundation_controller_->statusText() : QString{};
}

void EditController::setFoundationAiDenoiseEnabled(const bool enabled) {
    if (raw_foundation_controller_) {
        raw_foundation_controller_->setEnabled(enabled);
    }
}

void EditController::setFoundationAiDenoiseAmount(const int amount_percent) {
    if (!active_ || interactionLocked() || amount_percent < 0 || amount_percent > 100
        || grade_stack_.raw_ai_denoise.amount_percent
               == static_cast<std::uint8_t>(amount_percent)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.raw_ai_denoise.amount_percent =
        static_cast<std::uint8_t>(amount_percent);
    rawDenoiseEdited(QStringLiteral("amount"), before);
}

void EditController::startFoundationAiDenoise() {
    if (raw_foundation_controller_) {
        raw_foundation_controller_->start();
    }
}

void EditController::cancelFoundationAiDenoise() {
    if (raw_foundation_controller_) {
        raw_foundation_controller_->cancel();
    }
}

bool EditRawFoundationController::enabled() const noexcept {
    return state_.recipe_enabled();
}

bool EditRawFoundationController::available() const noexcept {
    return state_.runtime_available();
}

bool EditRawFoundationController::busy() const noexcept {
    return probe_watcher_.isRunning() || execution_watcher_.isRunning() || state_.job_busy();
}

bool EditRawFoundationController::canStart() const noexcept {
    return state_.active() && !state_.recipe_enabled() && !busy() && !owner_.interactionLocked();
}

bool EditRawFoundationController::canCancel() const noexcept {
    return state_.job_busy() && !state_.cancellation_requested();
}

QString EditRawFoundationController::phase() const {
    return edit_phase_code(state_.phase());
}

double EditRawFoundationController::progress() const noexcept {
    return std::clamp(static_cast<double>(state_.completed_basis_points()) / 10'000.0, 0.0, 1.0);
}

QString EditRawFoundationController::statusText() const {
    return status_message_.translated();
}

void EditRawFoundationController::setEnabled(const bool enabled) {
    if (enabled) {
        start();
        return;
    }
    if (!state_.recipe_enabled() || !owner_.active_ || owner_.interactionLocked()) {
        return;
    }
    const BackendGradeStack before = owner_.grade_stack_;
    owner_.grade_stack_.raw_ai_denoise.enabled = false;
    owner_.rawDenoiseEdited(QStringLiteral("enabled"), before);
    static_cast<void>(state_.sync_recipe_enabled(false));
    setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
        "EditController",
        "AI RAW Denoise bypassed · the verified foundation remains cached"
    )));
    publishChange();
}

void EditRawFoundationController::start() {
    if (!state_.active() || state_.recipe_enabled() || owner_.interactionLocked()) {
        return;
    }
    if (state_.materialized_ready()) {
        pending_recipe_enable_ = true;
        maybeApplyReady();
        return;
    }
    if (probe_watcher_.isRunning()) {
        start_after_probe_ = true;
        setStatus(raw_foundation_message(
            QT_TRANSLATE_NOOP("EditController", "Checking the local AI RAW Denoise model…")
        ));
        publishChange();
        return;
    }
    if (!state_.runtime_available()) {
        requestProbe(true);
        return;
    }
    if (execution_watcher_.isRunning()) {
        start_when_execution_idle_ = true;
        setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Waiting for the previous AI RAW Denoise task to stop…"
        )));
        publishChange();
        return;
    }
    startJob();
}

void EditRawFoundationController::cancel() {
    const auto token = state_.request_cancellation();
    if (!token) {
        return;
    }
    try {
        backend_->cancelRawFoundationJob(*token);
    } catch (const std::exception& error) {
        setStatus(raw_foundation_message(
            QT_TRANSLATE_NOOP("EditController", "Could not cancel AI RAW Denoise · %1"),
            {QString::fromUtf8(error.what())}
        ));
        publishChange();
        return;
    }
    setStatus(
        raw_foundation_message(QT_TRANSLATE_NOOP("EditController", "Cancelling AI RAW Denoise…"))
    );
    publishChange();
}

void EditRawFoundationController::resetContext() {
    const bool active =
        owner_.active_ && !owner_.photo_id_.isEmpty() && !owner_.source_path_.isEmpty();
    const bool recipe_enabled = active && owner_.grade_stack_.raw_ai_denoise.enabled;
    if (const auto token = state_.reset_context(active, recipe_enabled)) {
        try {
            backend_->cancelRawFoundationJob(*token);
        } catch (...) {}
    }
    progress_timer_.stop();
    pending_recipe_enable_ = false;
    start_after_probe_ = false;
    start_when_execution_idle_ = false;
    probe_requested_ = active;
    setStatus(
        active
            ? raw_foundation_message(
                  QT_TRANSLATE_NOOP("EditController", "Checking the local AI RAW Denoise model…")
              )
            : raw_foundation_message(QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise is off"))
    );
    publishChange();
    if (active && !probe_watcher_.isRunning()) {
        requestProbe(false);
    }
}

void EditRawFoundationController::retranslateUi() {
    publishChange();
}

void EditRawFoundationController::requestProbe(const bool start_after_probe) {
    if (!state_.active()) {
        return;
    }
    start_after_probe_ = start_after_probe_ || start_after_probe;
    probe_requested_ = true;
    if (probe_watcher_.isRunning()) {
        return;
    }
    probe_requested_ = false;
    const std::uint64_t context_generation = state_.begin_probe();
    setStatus(raw_foundation_message(
        QT_TRANSLATE_NOOP("EditController", "Checking the local AI RAW Denoise model…")
    ));
    publishChange();
    probe_watcher_.setFuture(QtConcurrent::run(probe_raw_foundation, backend_, context_generation));
}

void EditRawFoundationController::finishProbe() {
    const EditRawFoundationProbeTaskResult task = probe_watcher_.result();
    const bool available = task.error.isEmpty() && task.status.available;
    if (!state_.complete_probe(task.context_generation, available)) {
        if (probe_requested_ && state_.active()) {
            requestProbe(false);
        }
        return;
    }

    const QString diagnostic = !task.error.isEmpty() ? task.error : task.status.diagnostic;
    if (available) {
        if (state_.recipe_enabled()) {
            setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Saved AI RAW Denoise is enabled · the foundation is verified when rendered"
            )));
        } else {
            setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
                "EditController",
                "AI RAW Denoise is available · it runs once and remains reversible"
            )));
        }
    } else if (state_.recipe_enabled()) {
        setStatus(raw_foundation_message(
            QT_TRANSLATE_NOOP(
                "EditController",
                "Saved AI RAW Denoise is enabled, but the required local model is unavailable · %1"
            ),
            {diagnostic}
        ));
    } else {
        setStatus(raw_foundation_message(
            QT_TRANSLATE_NOOP(
                "EditController",
                "AI RAW Denoise is unavailable · install the verified local model, then retry · %1"
            ),
            {diagnostic}
        ));
    }
    const bool should_start = start_after_probe_;
    start_after_probe_ = false;
    publishChange();
    if (should_start && available) {
        startJob();
    }
}

void EditRawFoundationController::startJob() {
    if (!state_.can_start_job() || execution_watcher_.isRunning()) {
        return;
    }
    const std::uint64_t context_generation = state_.context_generation();
    const QString request_id = QStringLiteral("raw-foundation:%1:%2:%3")
                                   .arg(owner_.photo_id_)
                                   .arg(context_generation)
                                   .arg(++request_sequence_);
    std::uint64_t job_token = 0;
    try {
        job_token = backend_->beginRawFoundationJob(request_id, context_generation);
    } catch (const std::exception& error) {
        setStatus(raw_foundation_message(
            QT_TRANSLATE_NOOP("EditController", "Could not start AI RAW Denoise · %1"),
            {QString::fromUtf8(error.what())}
        ));
        publishChange();
        return;
    }
    if (!state_.begin_job(job_token, context_generation)) {
        try {
            backend_->cancelRawFoundationJob(job_token);
        } catch (...) {}
        return;
    }

    const QString photo_id = owner_.photo_id_;
    const QString source_path = owner_.source_path_;
    execution_watcher_.setFuture(
        QtConcurrent::run(
            execute_raw_foundation,
            backend_,
            photo_id,
            source_path,
            job_token,
            context_generation
        )
    );
    progress_timer_.start();
    setStatus(
        raw_foundation_message(QT_TRANSLATE_NOOP("EditController", "Preparing AI RAW Denoise…"))
    );
    publishChange();
}

void EditRawFoundationController::pollJob() {
    const auto token = state_.active_job_token();
    if (!token) {
        progress_timer_.stop();
        return;
    }
    BackendRawFoundationJobStatus status;
    try {
        status = backend_->rawFoundationJobStatus(*token);
    } catch (...) {
        return;
    }
    if (status.terminal()) {
        progress_timer_.stop();
        return;
    }
    const auto acceptance = state_.accept_job_status(
        status.job_token,
        status.generation,
        edit_phase(status.phase),
        status.completed_basis_points,
        status.cancellation_requested
    );
    if (acceptance == shadow::desktop::EditRawFoundationJobAcceptance::Stale) {
        return;
    }
    if (status.phase == BackendRawFoundationJobPhase::Planning) {
        setStatus(
            raw_foundation_message(QT_TRANSLATE_NOOP("EditController", "Planning AI RAW Denoise…"))
        );
    } else {
        setStatus(raw_foundation_message(
            QT_TRANSLATE_NOOP("EditController", "Running AI RAW Denoise · %1%"),
            {static_cast<int>(status.completed_basis_points / 100U)}
        ));
    }
    publishChange();
}

void EditRawFoundationController::finishExecution() {
    progress_timer_.stop();
    const EditRawFoundationExecutionTaskResult task = execution_watcher_.result();
    if (!task.error.isEmpty()) {
        try {
            backend_->cancelRawFoundationJob(task.job_token);
        } catch (...) {}
        const bool current = state_.fail_active_job(task.job_token, task.context_generation);
        try {
            const auto status = backend_->rawFoundationJobStatus(task.job_token);
            retireTerminal(status);
        } catch (...) {}
        if (current) {
            setStatus(raw_foundation_message(
                QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise failed · %1"),
                {task.error}
            ));
            publishChange();
        }
    } else {
        const auto acceptance = state_.accept_job_status(
            task.status.job_token,
            task.status.generation,
            edit_phase(task.status.phase),
            task.status.completed_basis_points,
            task.status.cancellation_requested
        );
        retireTerminal(task.status);
        if (acceptance != shadow::desktop::EditRawFoundationJobAcceptance::Stale
            && contextIsCurrent(task.context_generation, task.photo_id, task.source_path)) {
            switch (task.status.phase) {
            case BackendRawFoundationJobPhase::Ready:
                pending_recipe_enable_ = true;
                setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
                    "EditController",
                    "AI RAW Denoise foundation is ready · enabling the non-destructive switch…"
                )));
                publishChange();
                maybeApplyReady();
                break;
            case BackendRawFoundationJobPhase::Unavailable:
                setStatus(raw_foundation_message(
                    QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise is unavailable · %1"),
                    {task.status.diagnostic}
                ));
                publishChange();
                break;
            case BackendRawFoundationJobPhase::Cancelled:
                setStatus(raw_foundation_message(
                    QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise cancelled")
                ));
                publishChange();
                break;
            case BackendRawFoundationJobPhase::Failed:
                setStatus(raw_foundation_message(
                    QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise failed · %1"),
                    {task.status.diagnostic}
                ));
                publishChange();
                break;
            case BackendRawFoundationJobPhase::Queued:
            case BackendRawFoundationJobPhase::Planning:
            case BackendRawFoundationJobPhase::Running:
                setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
                    "EditController",
                    "AI RAW Denoise ended without a terminal result"
                )));
                publishChange();
                break;
            }
        }
    }

    if (start_when_execution_idle_ && state_.active()) {
        start_when_execution_idle_ = false;
        start();
    }
}

void EditRawFoundationController::syncRecipeState() {
    const bool enabled = owner_.active_ && owner_.grade_stack_.raw_ai_denoise.enabled;
    if (!state_.sync_recipe_enabled(enabled)) {
        return;
    }
    if (enabled) {
        pending_recipe_enable_ = false;
        setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI RAW Denoise is enabled"
        )));
    } else if (!state_.job_busy()) {
        setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI RAW Denoise is bypassed · other adjustments remain editable"
        )));
    }
    publishChange();
}

void EditRawFoundationController::maybeApplyReady() {
    if (!pending_recipe_enable_ || !state_.active() || !state_.materialized_ready()
        || !contextIsCurrent(state_.context_generation(), owner_.photo_id_, owner_.source_path_)) {
        return;
    }
    if (owner_.interactionLocked()) {
        setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
            "EditController",
            "AI RAW Denoise is ready · waiting for the current edit transaction…"
        )));
        publishChange();
        return;
    }
    if (!owner_.grade_stack_.raw_ai_denoise.enabled) {
        const BackendGradeStack before = owner_.grade_stack_;
        owner_.grade_stack_.raw_ai_denoise.model = 0;
        owner_.grade_stack_.raw_ai_denoise.enabled = true;
        owner_.rawDenoiseEdited(QStringLiteral("enabled"), before);
    }
    pending_recipe_enable_ = false;
    static_cast<void>(state_.sync_recipe_enabled(true));
    setStatus(raw_foundation_message(QT_TRANSLATE_NOOP(
        "EditController",
        "AI RAW Denoise enabled · Undo and bypass remain available"
    )));
    publishChange();
}

void EditRawFoundationController::publishChange() {
    emit owner_.foundationAiDenoiseChanged();
}

void EditRawFoundationController::setStatus(LocalizedUiMessage status) {
    if (status_message_ == status) {
        return;
    }
    status_message_ = std::move(status);
}

void EditRawFoundationController::retireTerminal(
    const BackendRawFoundationJobStatus& status
) const noexcept {
    if (status.job_token == 0 || !status.terminal()) {
        return;
    }
    try {
        backend_->retireRawFoundationJob(status.job_token);
    } catch (...) {}
}

bool EditRawFoundationController::contextIsCurrent(
    const std::uint64_t context_generation,
    const QString& photo_id,
    const QString& source_path
) const noexcept {
    return state_.active() && state_.context_generation() == context_generation && owner_.active_
           && owner_.photo_id_ == photo_id && owner_.source_path_ == source_path;
}
