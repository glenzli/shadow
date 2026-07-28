#include "review_source_health_coordinator.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t MISSING_SOURCE_LOCATION_PAGE_SIZE = 24;

[[nodiscard]] LocalizedUiMessage source_health_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewSourceHealthCoordinator::ReviewSourceHealthCoordinator(
    Operations operations,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)) {
    if (!operations_.source_health || !operations_.missing_locations
        || !operations_.relink) {
        throw std::invalid_argument(
            "complete Review source-health operations are required"
        );
    }
    connect(
        &source_health_watcher_,
        &QFutureWatcher<SourceHealthTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishSourceHealthTask
    );
    connect(
        &missing_locations_watcher_,
        &QFutureWatcher<MissingLocationTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishMissingLocationTask
    );
    connect(
        &relink_watcher_,
        &QFutureWatcher<RelinkTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishRelinkTask
    );
}

ReviewSourceHealthCoordinator::~ReviewSourceHealthCoordinator() {
    source_health_watcher_.waitForFinished();
    missing_locations_watcher_.waitForFinished();
    relink_watcher_.waitForFinished();
}

QVariantList ReviewSourceHealthCoordinator::sourceHealth() const {
    QVariantList result;
    result.reserve(source_health_.size());
    for (const auto& source : source_health_) {
        result.push_back(QVariantMap{
            {QStringLiteral("sourceId"), source.source_id},
            {QStringLiteral("sourcePath"), source.source_display_path},
            {QStringLiteral("enabled"), source.source_enabled},
            {
                QStringLiteral("hasLatestCompletedScan"),
                source.has_latest_completed_scan,
            },
            {QStringLiteral("scanSessionId"), source.scan_session_id},
            {QStringLiteral("scanCompletedAtMs"), source.scan_completed_at_ms},
            {QStringLiteral("knownLocations"), source.known_locations},
            {QStringLiteral("seenLocations"), source.seen_locations},
            {QStringLiteral("notSeenLocations"), source.not_seen_locations},
        });
    }
    return result;
}

bool ReviewSourceHealthCoordinator::sourceHealthBusy() const noexcept {
    return source_health_running_;
}

QVariantList ReviewSourceHealthCoordinator::missingLocations() const {
    QVariantList result;
    result.reserve(missing_locations_.size());
    for (const auto& location : missing_locations_) {
        result.push_back(QVariantMap{
            {QStringLiteral("locationId"), location.location_id},
            {QStringLiteral("photoId"), location.photo_id},
            {QStringLiteral("title"), location.title},
            {QStringLiteral("sourcePath"), location.source_display_path},
            {QStringLiteral("hasCapturedAt"), location.has_captured_at},
            {
                QStringLiteral("capturedAtUnixSeconds"),
                location.captured_at_unix_seconds,
            },
            {QStringLiteral("cameraKey"), location.camera_key},
            {QStringLiteral("lastSeenAtMs"), location.last_seen_at_ms},
        });
    }
    return result;
}

QString ReviewSourceHealthCoordinator::missingLocationScanId() const {
    return missing_location_scan_id_;
}

bool ReviewSourceHealthCoordinator::missingLocationsBusy() const noexcept {
    return missing_locations_running_;
}

bool ReviewSourceHealthCoordinator::missingLocationsHasMore() const noexcept {
    return missing_locations_has_more_;
}

bool ReviewSourceHealthCoordinator::relinkBusy() const noexcept {
    return relink_running_;
}

QString ReviewSourceHealthCoordinator::relinkStatusText() const {
    return relink_status_message_.translated();
}

LocalizedUiMessage
ReviewSourceHealthCoordinator::globalStatusMessage() const {
    return global_status_message_;
}

void ReviewSourceHealthCoordinator::refreshSourceHealth() {
    if (source_health_running_) {
        source_health_refresh_pending_ = true;
        return;
    }
    startSourceHealthTask();
}

void ReviewSourceHealthCoordinator::openMissingLocationReview(
    const QString& scan_session_id
) {
    const QString normalized_scan_id = scan_session_id.trimmed();
    if (normalized_scan_id.isEmpty()) {
        return;
    }
    missing_location_scan_id_ = normalized_scan_id;
    missing_location_next_cursor_.clear();
    missing_locations_.clear();
    missing_locations_has_more_ = false;
    if (missing_locations_running_) {
        active_missing_locations_request_id_ = ++missing_locations_request_id_;
        missing_locations_refresh_pending_ = true;
        emit missingLocationReviewChanged();
        return;
    }
    startMissingLocationTask(false);
}

void ReviewSourceHealthCoordinator::closeMissingLocationReview() {
    active_missing_locations_request_id_ = ++missing_locations_request_id_;
    missing_locations_refresh_pending_ = false;
    missing_location_scan_id_.clear();
    missing_location_next_cursor_.clear();
    missing_locations_.clear();
    missing_locations_has_more_ = false;
    emit missingLocationReviewChanged();
}

void ReviewSourceHealthCoordinator::loadMoreMissingLocations() {
    if (missing_locations_running_ || !missing_locations_has_more_
        || missing_location_scan_id_.isEmpty()) {
        return;
    }
    startMissingLocationTask(true);
}

void ReviewSourceHealthCoordinator::relinkMissingLocation(
    const QString& location_id,
    const QUrl& candidate_url
) {
    const QString normalized_location_id = location_id.trimmed();
    const QString candidate_path = candidate_url.toLocalFile();
    if (relink_running_ || missing_location_scan_id_.isEmpty()
        || normalized_location_id.isEmpty() || candidate_path.isEmpty()) {
        return;
    }
    startRelinkTask(normalized_location_id, candidate_path);
}

void ReviewSourceHealthCoordinator::retranslateUi() {
    if (!relink_status_message_.isEmpty()) {
        emit missingLocationReviewChanged();
    }
}

ReviewSourceHealthCoordinator::SourceHealthTaskResult
ReviewSourceHealthCoordinator::runSourceHealthTask(
    std::function<QVector<BackendLibrarySourceHealth>()> operation,
    const quint64 request_id
) {
    SourceHealthTaskResult result;
    result.request_id = request_id;
    try {
        result.sources = operation();
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::MissingLocationTaskResult
ReviewSourceHealthCoordinator::runMissingLocationTask(
    std::function<BackendMissingSourceLocationPage(
        const QString& scan_session_id,
        const QString& after_location_id,
        std::uint32_t limit
    )> operation,
    QString scan_session_id,
    QString after_location_id,
    const quint64 request_id,
    const bool append
) {
    MissingLocationTaskResult result;
    result.scan_session_id = std::move(scan_session_id);
    result.request_id = request_id;
    result.append = append;
    try {
        result.page = operation(
            result.scan_session_id,
            after_location_id,
            MISSING_SOURCE_LOCATION_PAGE_SIZE
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::RelinkTaskResult
ReviewSourceHealthCoordinator::runRelinkTask(
    std::function<BackendVerifiedSourceRelinkReceipt(
        const QString& scan_session_id,
        const QString& location_id,
        const QString& candidate_path
    )> operation,
    QString scan_session_id,
    QString location_id,
    QString candidate_path,
    const quint64 request_id
) {
    RelinkTaskResult result;
    result.location_id = location_id;
    result.request_id = request_id;
    try {
        result.receipt = operation(
            scan_session_id,
            location_id,
            candidate_path
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewSourceHealthCoordinator::startSourceHealthTask() {
    if (source_health_running_) {
        source_health_refresh_pending_ = true;
        return;
    }
    source_health_running_ = true;
    active_source_health_request_id_ = ++source_health_request_id_;
    emit sourceHealthChanged();
    source_health_watcher_.setFuture(QtConcurrent::run(
        runSourceHealthTask,
        operations_.source_health,
        active_source_health_request_id_
    ));
}

void ReviewSourceHealthCoordinator::startMissingLocationTask(
    const bool append
) {
    if (missing_locations_running_) {
        missing_locations_refresh_pending_ = true;
        return;
    }
    if (missing_location_scan_id_.isEmpty()) {
        return;
    }
    missing_locations_running_ = true;
    active_missing_locations_request_id_ = ++missing_locations_request_id_;
    emit missingLocationReviewChanged();
    missing_locations_watcher_.setFuture(QtConcurrent::run(
        runMissingLocationTask,
        operations_.missing_locations,
        missing_location_scan_id_,
        append ? missing_location_next_cursor_ : QString{},
        active_missing_locations_request_id_,
        append
    ));
}

void ReviewSourceHealthCoordinator::startRelinkTask(
    const QString& location_id,
    const QString& candidate_path
) {
    relink_running_ = true;
    relink_status_message_ = source_health_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Verifying selected source…"
    ));
    active_relink_request_id_ = ++relink_request_id_;
    emit missingLocationReviewChanged();
    relink_watcher_.setFuture(QtConcurrent::run(
        runRelinkTask,
        operations_.relink,
        missing_location_scan_id_,
        location_id,
        candidate_path,
        active_relink_request_id_
    ));
}

void ReviewSourceHealthCoordinator::finishSourceHealthTask() {
    SourceHealthTaskResult result = source_health_watcher_.result();
    source_health_running_ = false;
    const bool accepted =
        result.request_id == active_source_health_request_id_;
    if (accepted && result.error.isEmpty()) {
        source_health_ = std::move(result.sources);
        emit sourceHealthChanged();
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not load Library source health · %1"
            ),
            {result.error}
        ));
        emit sourceHealthChanged();
    }

    if (source_health_refresh_pending_ || !accepted) {
        source_health_refresh_pending_ = false;
        startSourceHealthTask();
    }
}

void ReviewSourceHealthCoordinator::finishMissingLocationTask() {
    MissingLocationTaskResult result = missing_locations_watcher_.result();
    missing_locations_running_ = false;
    const bool accepted =
        result.request_id == active_missing_locations_request_id_
        && result.scan_session_id == missing_location_scan_id_;
    if (accepted && result.error.isEmpty()) {
        if (result.page.has_scan) {
            if (result.append) {
                missing_locations_ += std::move(result.page.items);
            } else {
                missing_locations_ = std::move(result.page.items);
            }
            missing_location_next_cursor_ =
                std::move(result.page.next_location_id);
            missing_locations_has_more_ = result.page.has_more;
        } else {
            missing_locations_.clear();
            missing_location_next_cursor_.clear();
            missing_locations_has_more_ = false;
        }
        emit missingLocationReviewChanged();
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not load source scan review · %1"
            ),
            {result.error}
        ));
        emit missingLocationReviewChanged();
    } else {
        // Switching or closing the review invalidates the page while its
        // worker remains cooperative and bounded.
        emit missingLocationReviewChanged();
    }

    if (missing_locations_refresh_pending_
        && !missing_location_scan_id_.isEmpty()) {
        missing_locations_refresh_pending_ = false;
        startMissingLocationTask(false);
    }
}

void ReviewSourceHealthCoordinator::finishRelinkTask() {
    RelinkTaskResult result = relink_watcher_.result();
    relink_running_ = false;
    const bool accepted = result.request_id == active_relink_request_id_;
    if (accepted && result.error.isEmpty()) {
        relink_status_message_ = source_health_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Verified and linked · %1"
            ),
            {result.receipt.display_path}
        );
        publishGlobalStatus(relink_status_message_);
    } else if (accepted) {
        relink_status_message_ = source_health_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not link selected source · %1"
            ),
            {result.error}
        );
        publishGlobalStatus(relink_status_message_);
    }
    emit missingLocationReviewChanged();
}

void ReviewSourceHealthCoordinator::publishGlobalStatus(
    LocalizedUiMessage status
) {
    global_status_message_ = std::move(status);
    emit globalStatusMessageChanged();
}
