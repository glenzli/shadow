#include "edit_neighbor_preheater.hpp"

#include "edit_controller.hpp"
#include "justified_review_layout_model.hpp"

#include <QtConcurrent>

#include <QDebug>
#include <QElapsedTimer>
#include <QThread>

#include <exception>
#include <utility>

namespace {

constexpr int EDIT_NEIGHBOR_IDLE_MS = 220;
constexpr std::uint32_t EDIT_NEIGHBOR_PREVIEW_EDGE = 1'536;

[[nodiscard]] QString targetValue(const QVariantMap& target, const char* key) {
    return target.value(QString::fromLatin1(key)).toString();
}

[[nodiscard]] bool sameTarget(const QVariantMap& left, const QVariantMap& right) {
    return targetValue(left, "photoId") == targetValue(right, "photoId")
           && targetValue(left, "representationId") == targetValue(right, "representationId")
           && targetValue(left, "sourcePath") == targetValue(right, "sourcePath");
}

[[nodiscard]] bool localEditable(const QVariantMap& target) {
    return !targetValue(target, "photoId").isEmpty()
           && !targetValue(target, "representationId").isEmpty()
           && !targetValue(target, "sourcePath").isEmpty()
           && target.value(QStringLiteral("sourceAvailable")).toBool()
           && !target.value(QStringLiteral("isRemote")).toBool();
}

[[nodiscard]] bool diagnosticsEnabled() {
    return qEnvironmentVariable("SHADOW_EDIT_PREFETCH_DIAGNOSTICS") == QStringLiteral("1");
}

} // namespace

EditNeighborPreheater::EditNeighborPreheater(
    std::shared_ptr<DesktopBackend> backend,
    EditController& editor,
    JustifiedReviewLayoutModel& navigation,
    QObject* parent
) : QObject(parent), backend_(std::move(backend)), editor_(editor), navigation_(navigation) {
    pool_.setMaxThreadCount(1);
    pool_.setThreadPriority(QThread::LowPriority);
    idle_timer_.setSingleShot(true);
    idle_timer_.setInterval(EDIT_NEIGHBOR_IDLE_MS);
    connect(&idle_timer_, &QTimer::timeout, this, &EditNeighborPreheater::startIfIdle);
    connect(
        &watcher_,
        &QFutureWatcher<WorkResult>::finished,
        this,
        &EditNeighborPreheater::finishWork
    );
    connect(
        &editor_,
        &EditController::sourceIdentityChanged,
        this,
        &EditNeighborPreheater::refreshTargets
    );
    connect(&editor_, &EditController::activeChanged, this, &EditNeighborPreheater::refreshTargets);
    connect(
        &navigation_,
        &JustifiedReviewLayoutModel::layoutChanged,
        this,
        &EditNeighborPreheater::refreshTargets
    );
    connect(
        &navigation_,
        &JustifiedReviewLayoutModel::sourceModelChanged,
        this,
        &EditNeighborPreheater::refreshTargets
    );
    connect(
        &navigation_,
        &QAbstractItemModel::modelReset,
        this,
        &EditNeighborPreheater::refreshTargets
    );
    connect(&navigation_, &QAbstractItemModel::rowsInserted, this, [this]() { refreshTargets(); });
    connect(&navigation_, &QAbstractItemModel::dataChanged, this, [this]() { refreshTargets(); });
    for (const auto signal : {
             &EditController::busyChanged,
             &EditController::autosavePendingChanged,
             &EditController::fullResolutionStateChanged,
             &EditController::previewSourceChanged,
         }) {
        connect(&editor_, signal, this, &EditNeighborPreheater::updateAdmission);
    }
    refreshTargets();
}

EditNeighborPreheater::~EditNeighborPreheater() {
    idle_timer_.stop();
    cancelWork();
    watcher_.waitForFinished();
    pool_.waitForDone();
    editor_.setNeighborPreviewPreparationPending(false);
}

QVariantMap EditNeighborPreheater::nextTarget() const {
    return next_target_;
}
QVariantMap EditNeighborPreheater::previousTarget() const {
    return previous_target_;
}
EditNeighborPreheater::State EditNeighborPreheater::state() const noexcept {
    return state_;
}
bool EditNeighborPreheater::preparing() const noexcept {
    return state_ == State::Preparing;
}
bool EditNeighborPreheater::prepared() const noexcept {
    return state_ == State::Prepared;
}
bool EditNeighborPreheater::atLoadedEnd() const noexcept {
    return at_loaded_end_;
}
bool EditNeighborPreheater::enabled() const noexcept {
    return enabled_;
}

void EditNeighborPreheater::setEnabled(const bool enabled) {
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;
    emit enabledChanged();
    refreshTargets();
}

void EditNeighborPreheater::refreshTargets() {
    QVariantMap next;
    QVariantMap previous;
    QString origin_photo;
    QString origin_representation;
    bool current_present = false;
    if (enabled_ && editor_.active()) {
        origin_photo = editor_.photoId();
        origin_representation = editor_.representationId();
        const QVariantMap current =
            navigation_.navigationTarget(origin_photo, origin_representation, 0, 0);
        // navigationTarget falls back to the first row for an unknown origin.
        // That fallback is useful for Review navigation, but never establishes
        // an adjacent edit session after a filter or album change.
        if (targetValue(current, "photoId") == origin_photo
            && targetValue(current, "representationId") == origin_representation) {
            current_present = true;
            next = navigation_.navigationTarget(origin_photo, origin_representation, 1, 0);
            previous = navigation_.navigationTarget(origin_photo, origin_representation, -1, 0);
        }
    }
    const bool origin_changed =
        origin_photo != origin_photo_id_ || origin_representation != origin_representation_id_;
    const bool next_changed =
        !sameTarget(next, next_target_) || localEditable(next) != localEditable(next_target_);
    const bool loaded_end = current_present && targetValue(next, "photoId").isEmpty();
    if (origin_changed || next_changed) {
        idle_timer_.stop();
        cancelWork();
        setState(localEditable(next) ? State::Waiting : State::Unavailable);
    }
    origin_photo_id_ = std::move(origin_photo);
    origin_representation_id_ = std::move(origin_representation);
    if (next != next_target_ || previous != previous_target_ || loaded_end != at_loaded_end_) {
        next_target_ = std::move(next);
        previous_target_ = std::move(previous);
        at_loaded_end_ = loaded_end;
        emit targetsChanged();
    }
    updateDetailPriority();
    updateAdmission();
}

bool EditNeighborPreheater::canPrepare() const {
    return enabled_ && editor_.active() && localEditable(next_target_)
           && !editor_.previewSource().isEmpty() && !editor_.busy() && !editor_.autosavePending()
           && !editor_.autosaveFailed() && !editor_.fullResolutionPreparing();
}

void EditNeighborPreheater::updateAdmission() {
    if (!canPrepare()) {
        idle_timer_.stop();
        if (work_outstanding_) {
            cancelWork();
            setState(State::Waiting);
        }
        return;
    }
    if (state_ == State::Waiting && !work_outstanding_ && !idle_timer_.isActive())
        idle_timer_.start();
}

void EditNeighborPreheater::startIfIdle() {
    if (!canPrepare() || work_outstanding_ || state_ != State::Waiting)
        return;
    const QVariantMap target = next_target_;
    const QString photo_id = targetValue(target, "photoId");
    const QString representation_id = targetValue(target, "representationId");
    const QString source_path = targetValue(target, "sourcePath");
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    render_token_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    const auto cancellation = cancellation_;
    const auto token = render_token_;
    const auto backend = backend_;
    work_outstanding_ = true;
    setState(State::Preparing);
    watcher_.setFuture(
        QtConcurrent::run(
            &pool_,
            [backend, cancellation, token, photo_id, representation_id, source_path]() {
                WorkResult result{
                    .photo_id = photo_id,
                    .representation_id = representation_id,
                    .source_path = source_path,
                };
                QElapsedTimer elapsed;
                elapsed.start();
                try {
                    const BackendPhotoEditState state =
                        backend->photoEditState(photo_id, source_path);
                    if (cancellation->load(std::memory_order_acquire)) {
                        result.cancelled = true;
                    } else {
                        const std::uint64_t request_token = backend->beginEditPreviewRequest();
                        token->store(request_token, std::memory_order_release);
                        if (cancellation->load(std::memory_order_acquire))
                            (void)backend->cancelEditPreviewRequest(request_token);
                        const BackendEditedPreview preview = backend->renderEditPreview(
                            photo_id,
                            source_path,
                            state.base_commit_id,
                            state.grade_stack,
                            request_token,
                            EDIT_NEIGHBOR_PREVIEW_EDGE,
                            84,
                            EditPreviewPolicy::Interactive,
                            std::nullopt
                        );
                        result.cancelled = cancellation->load(std::memory_order_acquire)
                                           || preview.terminal == EditPreviewTerminal::Cancelled;
                        result.completed =
                            !result.cancelled && preview.terminal == EditPreviewTerminal::Completed;
                        token->store(0, std::memory_order_release);
                    }
                } catch (const std::exception& error) {
                    token->store(0, std::memory_order_release);
                    result.error = QString::fromUtf8(error.what());
                    result.cancelled = cancellation->load(std::memory_order_acquire);
                }
                result.elapsed_ms = elapsed.elapsed();
                return result;
            }
        )
    );
}

void EditNeighborPreheater::cancelWork() {
    if (!work_outstanding_)
        return;
    cancellation_->store(true, std::memory_order_release);
    const std::uint64_t token = render_token_->load(std::memory_order_acquire);
    if (token != 0)
        (void)backend_->cancelEditPreviewRequest(token);
}

bool EditNeighborPreheater::matchesNext(const WorkResult& result) const {
    return result.photo_id == targetValue(next_target_, "photoId")
           && result.representation_id == targetValue(next_target_, "representationId")
           && result.source_path == targetValue(next_target_, "sourcePath");
}

void EditNeighborPreheater::finishWork() {
    const WorkResult result = watcher_.result();
    work_outstanding_ = false;
    const bool cancelled_after_completion = cancellation_->load(std::memory_order_acquire);
    if (diagnosticsEnabled()) {
        qInfo().noquote() << QStringLiteral("shadow.edit-prefetch terminal=%1 elapsed_ms=%2")
                                 .arg(
                                     result.completed && !cancelled_after_completion
                                         ? QStringLiteral("prepared")
                                     : result.cancelled || cancelled_after_completion
                                         ? QStringLiteral("cancelled")
                                         : QStringLiteral("failed")
                                 )
                                 .arg(result.elapsed_ms);
    }
    if (!matchesNext(result) || !enabled_) {
        updateDetailPriority();
        updateAdmission();
        return;
    }
    if (result.completed && !cancelled_after_completion && canPrepare()) {
        setState(State::Prepared);
    } else if (result.completed || result.cancelled || cancelled_after_completion) {
        setState(State::Waiting);
        updateAdmission();
    } else {
        setState(State::Failed);
    }
}

void EditNeighborPreheater::setState(const State state) {
    if (state_ == state)
        return;
    state_ = state;
    emit stateChanged();
    updateDetailPriority();
}

void EditNeighborPreheater::updateDetailPriority() {
    editor_.setNeighborPreviewPreparationPending(
        enabled_ && localEditable(next_target_)
        && (state_ == State::Waiting || state_ == State::Preparing)
    );
}
