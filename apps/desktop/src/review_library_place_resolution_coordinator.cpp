#include "review_library_place_resolution_coordinator.hpp"

#include <QPointer>
#include <QtConcurrentRun>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t PLACE_CANDIDATE_PAGE_SIZE = 32;
constexpr int SUCCESS_PACING_MS = 200;

} // namespace

ReviewLibraryPlaceResolutionCoordinator::ReviewLibraryPlaceResolutionCoordinator(
    Operations operations,
    std::unique_ptr<LibraryReverseGeocoder> provider,
    QObject* const parent
) : QObject(parent), operations_(std::move(operations)), provider_(std::move(provider)) {
    if (!operations_.candidates || !operations_.record) {
        throw std::invalid_argument("all Library place-resolution operations are required");
    }
    if (!provider_) {
        throw std::invalid_argument("a Library reverse-geocoding provider is required");
    }
    next_timer_.setSingleShot(true);
    next_timer_.setInterval(SUCCESS_PACING_MS);
    next_timer_.setTimerType(Qt::CoarseTimer);
    connect(
        &next_timer_,
        &QTimer::timeout,
        this,
        &ReviewLibraryPlaceResolutionCoordinator::requestCandidates
    );
    connect(
        &candidate_watcher_,
        &QFutureWatcher<CandidateTaskResult>::finished,
        this,
        &ReviewLibraryPlaceResolutionCoordinator::finishCandidates
    );
    connect(
        &record_watcher_,
        &QFutureWatcher<RecordTaskResult>::finished,
        this,
        &ReviewLibraryPlaceResolutionCoordinator::finishRecord
    );
}

ReviewLibraryPlaceResolutionCoordinator::~ReviewLibraryPlaceResolutionCoordinator() {
    stopping_ = true;
    next_timer_.stop();
    provider_->cancel();
    candidate_watcher_.waitForFinished();
    record_watcher_.waitForFinished();
}

bool ReviewLibraryPlaceResolutionCoordinator::running() const noexcept {
    return running_;
}

std::uint64_t ReviewLibraryPlaceResolutionCoordinator::recordedCount() const noexcept {
    return recorded_count_;
}

void ReviewLibraryPlaceResolutionCoordinator::start() {
    if (!provider_->available() || stopping_) {
        return;
    }
    if (running_) {
        restart_requested_ = true;
        return;
    }
    failed_coordinates_.clear();
    running_ = true;
    emit stateChanged();
    requestCandidates();
}

ReviewLibraryPlaceResolutionCoordinator::CandidateTaskResult
ReviewLibraryPlaceResolutionCoordinator::runCandidateTask(
    Operations operations,
    const std::uint32_t limit
) {
    CandidateTaskResult result;
    try {
        result.candidates = operations.candidates(limit);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewLibraryPlaceResolutionCoordinator::RecordTaskResult
ReviewLibraryPlaceResolutionCoordinator::runRecordTask(
    Operations operations,
    BackendLibraryPlaceResolutionResult result
) {
    RecordTaskResult task;
    try {
        task.status = operations.record(result);
    } catch (const std::exception& error) {
        task.error = QString::fromUtf8(error.what());
    }
    return task;
}

QString ReviewLibraryPlaceResolutionCoordinator::coordinateKey(
    const BackendLibraryPlaceResolutionCandidate& candidate
) {
    return QStringLiteral("%1:%2").arg(candidate.latitude_e7).arg(candidate.longitude_e7);
}

void ReviewLibraryPlaceResolutionCoordinator::requestCandidates() {
    if (stopping_ || !running_ || candidate_watcher_.isRunning() || record_watcher_.isRunning()) {
        return;
    }
    candidate_watcher_.setFuture(
        QtConcurrent::run(runCandidateTask, operations_, PLACE_CANDIDATE_PAGE_SIZE)
    );
}

void ReviewLibraryPlaceResolutionCoordinator::finishCandidates() {
    if (stopping_ || !running_) {
        return;
    }
    CandidateTaskResult task = candidate_watcher_.result();
    if (!task.error.isEmpty()) {
        stopPass();
        return;
    }
    const auto iterator = std::find_if(
        task.candidates.cbegin(),
        task.candidates.cend(),
        [this](const BackendLibraryPlaceResolutionCandidate& candidate) {
            return !failed_coordinates_.contains(coordinateKey(candidate));
        }
    );
    if (iterator == task.candidates.cend()) {
        stopPass();
        return;
    }
    const BackendLibraryPlaceResolutionCandidate candidate = *iterator;
    const QPointer<ReviewLibraryPlaceResolutionCoordinator> guard(this);
    provider_->reverseGeocode(
        candidate,
        [guard, candidate](
            std::optional<BackendLibraryPlaceResolutionResult> result,
            QString error
        ) {
            if (guard) {
                guard->finishGeocode(candidate, std::move(result), error);
            }
        }
    );
}

void ReviewLibraryPlaceResolutionCoordinator::finishGeocode(
    const BackendLibraryPlaceResolutionCandidate& candidate,
    std::optional<BackendLibraryPlaceResolutionResult> result,
    const QString& error
) {
    if (stopping_ || !running_) {
        return;
    }
    if (!error.isEmpty() || !result) {
        failed_coordinates_.insert(coordinateKey(candidate));
        scheduleNext();
        return;
    }
    // Provider-returned coordinates are presentation data, never authority.
    result->latitude_e7 = candidate.latitude_e7;
    result->longitude_e7 = candidate.longitude_e7;
    record_watcher_.setFuture(QtConcurrent::run(runRecordTask, operations_, std::move(*result)));
}

void ReviewLibraryPlaceResolutionCoordinator::finishRecord() {
    if (stopping_ || !running_) {
        return;
    }
    const RecordTaskResult task = record_watcher_.result();
    if (!task.error.isEmpty()) {
        stopPass();
        return;
    }
    if (task.status == BackendRecordLibraryPlaceResolutionStatus::Recorded) {
        ++recorded_count_;
        emit placesChanged();
    }
    scheduleNext();
}

void ReviewLibraryPlaceResolutionCoordinator::scheduleNext() {
    if (!stopping_ && running_) {
        next_timer_.start();
    }
}

void ReviewLibraryPlaceResolutionCoordinator::stopPass() {
    if (!running_) {
        return;
    }
    running_ = false;
    next_timer_.stop();
    emit stateChanged();
    if (restart_requested_ && !stopping_) {
        restart_requested_ = false;
        start();
    }
}
