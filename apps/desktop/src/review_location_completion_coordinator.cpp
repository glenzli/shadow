#include "review_location_completion_coordinator.hpp"

#include <QVariantMap>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::int64_t kEventGapSeconds = 2 * 60 * 60;
constexpr std::uint32_t kPageSize = 256;
constexpr std::uint32_t kMaximumPhotos = 4'096;
constexpr double kAnchorSpreadMeters = 750.0;

struct Coordinate final {
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    QString place_name;
};

[[nodiscard]] double radians(const double degrees) {
    return degrees * 3.14159265358979323846 / 180.0;
}

[[nodiscard]] double coordinate_distance_meters(const Coordinate& left, const Coordinate& right) {
    constexpr double earth_radius_meters = 6'371'000.0;
    const double left_latitude = static_cast<double>(left.latitude_e7) / 10'000'000.0;
    const double right_latitude = static_cast<double>(right.latitude_e7) / 10'000'000.0;
    const double delta_latitude = radians(right_latitude - left_latitude);
    const double delta_longitude = radians(
        static_cast<double>(right.longitude_e7 - left.longitude_e7) / 10'000'000.0
    );
    const double sin_latitude = std::sin(delta_latitude / 2.0);
    const double sin_longitude = std::sin(delta_longitude / 2.0);
    const double a = sin_latitude * sin_latitude
        + std::cos(radians(left_latitude)) * std::cos(radians(right_latitude))
            * sin_longitude * sin_longitude;
    return earth_radius_meters * 2.0
        * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

struct EventAccumulator final {
    std::int64_t started_at_unix_seconds = 0;
    std::int64_t ended_at_unix_seconds = 0;
    QVector<BackendReviewItem> targets;
    QVector<Coordinate> anchors;
};

struct TimelineObservation final {
    std::int64_t captured_at_unix_seconds = 0;
    const BackendReviewItem* target = nullptr;
    std::optional<Coordinate> anchor;
};

[[nodiscard]] QVariantMap target_variant(const BackendReviewItem& item) {
    return {
        {QStringLiteral("photoId"), item.photo_id},
        {QStringLiteral("sourcePath"), item.source_path},
        {QStringLiteral("title"), item.title},
    };
}

[[nodiscard]] QVariantMap event_variant(
    const EventAccumulator& event,
    const int ordinal
) {
    QVariantList targets;
    targets.reserve(event.targets.size());
    for (const auto& target : event.targets) {
        targets.push_back(target_variant(target));
    }

    const auto& anchors = event.anchors;

    bool has_suggestion = !anchors.isEmpty();
    Coordinate centroid{};
    if (has_suggestion) {
        qint64 latitude_total = 0;
        qint64 longitude_total = 0;
        for (const auto& anchor : anchors) {
            latitude_total += anchor.latitude_e7;
            longitude_total += anchor.longitude_e7;
        }
        centroid.latitude_e7 = static_cast<std::int32_t>(
            latitude_total / static_cast<qint64>(anchors.size())
        );
        centroid.longitude_e7 = static_cast<std::int32_t>(
            longitude_total / static_cast<qint64>(anchors.size())
        );
        for (const auto& anchor : anchors) {
            if (coordinate_distance_meters(centroid, anchor) > kAnchorSpreadMeters) {
                has_suggestion = false;
                break;
            }
        }
        for (const auto& anchor : anchors) {
            if (!anchor.place_name.trimmed().isEmpty()) {
                centroid.place_name = anchor.place_name.trimmed();
                break;
            }
        }
    }

    return {
        {QStringLiteral("id"), QStringLiteral("event-%1-%2").arg(event.started_at_unix_seconds).arg(ordinal)},
        {QStringLiteral("startedAt"), event.started_at_unix_seconds},
        {QStringLiteral("endedAt"), event.ended_at_unix_seconds},
        {QStringLiteral("targetCount"), event.targets.size()},
        {QStringLiteral("anchorCount"), anchors.size()},
        {QStringLiteral("hasSuggestion"), has_suggestion},
        {QStringLiteral("latitude"), static_cast<double>(centroid.latitude_e7) / 10'000'000.0},
        {QStringLiteral("longitude"), static_cast<double>(centroid.longitude_e7) / 10'000'000.0},
        {QStringLiteral("placeName"), centroid.place_name},
        {QStringLiteral("targets"), targets},
    };
}

void append_event(
    QVariantList& result,
    const EventAccumulator& event,
    const int ordinal
) {
    if (!event.targets.isEmpty()) {
        result.push_back(event_variant(event, ordinal));
    }
}

} // namespace

ReviewLocationCompletionCoordinator::ReviewLocationCompletionCoordinator(
    Operations operations,
    QObject* const parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.page) {
        throw std::invalid_argument("the location-completion page operation is required");
    }
    if (!operations_.anchors) {
        throw std::invalid_argument("the location-completion anchor operation is required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLocationCompletionCoordinator::finishTask
    );
}

ReviewLocationCompletionCoordinator::~ReviewLocationCompletionCoordinator() {
    watcher_.waitForFinished();
}

QVariantList ReviewLocationCompletionCoordinator::groups() const {
    return groups_;
}

bool ReviewLocationCompletionCoordinator::busy() const noexcept {
    return task_running_;
}

bool ReviewLocationCompletionCoordinator::truncated() const noexcept {
    return truncated_;
}

QString ReviewLocationCompletionCoordinator::errorText() const {
    return error_text_;
}

void ReviewLocationCompletionCoordinator::request(
    BackendLibraryPhotoFilter filter,
    const std::int64_t capture_start_unix_seconds,
    const std::int64_t capture_end_unix_seconds
) {
    requested_filter_ = std::move(filter);
    requested_capture_start_unix_seconds_ = capture_start_unix_seconds;
    requested_capture_end_unix_seconds_ = capture_end_unix_seconds;
    ++request_id_;
    if (task_running_) {
        request_pending_ = true;
        return;
    }
    startTask();
}

void ReviewLocationCompletionCoordinator::invalidate() {
    groups_.clear();
    error_text_.clear();
    truncated_ = false;
    ++request_id_;
    emit stateChanged();
}

ReviewLocationCompletionCoordinator::TaskResult ReviewLocationCompletionCoordinator::runTask(
    Operations operations,
    BackendLibraryPhotoFilter filter,
    const std::int64_t capture_start_unix_seconds,
    const std::int64_t capture_end_unix_seconds,
    const quint64 request_id
) {
    TaskResult result;
    result.request_id = request_id;
    try {
        if (capture_start_unix_seconds > 0) {
            filter.has_capture_start = true;
            filter.capture_start_unix_seconds = capture_start_unix_seconds;
        }
        if (capture_end_unix_seconds > 0) {
            filter.has_capture_end = true;
            filter.capture_end_unix_seconds = capture_end_unix_seconds;
        }
        const auto reference_anchors = operations.anchors(
            capture_start_unix_seconds, capture_end_unix_seconds
        );

        BackendLibraryPhotoCursor cursor;
        QVector<BackendReviewItem> photos;
        bool has_more = true;
        while (has_more && photos.size() < static_cast<qsizetype>(kMaximumPhotos)) {
            const auto remaining = static_cast<std::uint32_t>(
                static_cast<qsizetype>(kMaximumPhotos) - photos.size()
            );
            const auto page = operations.page(
                filter,
                BackendLibraryPhotoOrder::CaptureTimeAscending,
                cursor,
                std::min(kPageSize, remaining)
            );
            photos += page.items;
            has_more = page.has_more;
            cursor = page.next_cursor;
            if (page.items.isEmpty()) {
                break;
            }
        }
        result.truncated = has_more;

        QVector<TimelineObservation> timeline;
        timeline.reserve(photos.size() + reference_anchors.size());
        for (const auto& photo : photos) {
            if (photo.captured_at_unix_seconds <= 0) {
                continue;
            }
            if (photo.has_coordinates) {
                timeline.push_back({
                    .captured_at_unix_seconds = photo.captured_at_unix_seconds,
                    .anchor = Coordinate{
                        .latitude_e7 = photo.latitude_e7,
                        .longitude_e7 = photo.longitude_e7,
                        .place_name = photo.place_name,
                    },
                });
            } else {
                timeline.push_back({
                    .captured_at_unix_seconds = photo.captured_at_unix_seconds,
                    .target = &photo,
                });
            }
        }
        for (const auto& reference : reference_anchors) {
            if (reference.captured_at_unix_seconds <= 0) {
                continue;
            }
            timeline.push_back({
                .captured_at_unix_seconds = reference.captured_at_unix_seconds,
                .anchor = Coordinate{
                    .latitude_e7 = reference.latitude_e7,
                    .longitude_e7 = reference.longitude_e7,
                },
            });
        }
        std::sort(
            timeline.begin(),
            timeline.end(),
            [](const TimelineObservation& left, const TimelineObservation& right) {
                return left.captured_at_unix_seconds < right.captured_at_unix_seconds;
            }
        );

        EventAccumulator event;
        int ordinal = 0;
        for (const auto& observation : timeline) {
            if (event.started_at_unix_seconds > 0
                && observation.captured_at_unix_seconds - event.ended_at_unix_seconds
                    > kEventGapSeconds) {
                append_event(result.groups, event, ordinal++);
                event = {};
            }
            if (event.started_at_unix_seconds == 0) {
                event.started_at_unix_seconds = observation.captured_at_unix_seconds;
            }
            event.ended_at_unix_seconds = observation.captured_at_unix_seconds;
            if (observation.target != nullptr) {
                event.targets.push_back(*observation.target);
            }
            if (observation.anchor.has_value())
                event.anchors.push_back(*observation.anchor);
        }
        append_event(result.groups, event, ordinal);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewLocationCompletionCoordinator::startTask() {
    task_running_ = true;
    error_text_.clear();
    active_request_id_ = request_id_;
    emit stateChanged();
    watcher_.setFuture(
        QtConcurrent::run(
            runTask,
            operations_,
            requested_filter_,
            requested_capture_start_unix_seconds_,
            requested_capture_end_unix_seconds_,
            active_request_id_
        )
    );
}

void ReviewLocationCompletionCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool accepted = result.request_id == request_id_;
    if (accepted) {
        if (result.error.isEmpty()) {
            groups_ = std::move(result.groups);
            truncated_ = result.truncated;
            error_text_.clear();
        } else {
            groups_.clear();
            truncated_ = false;
            error_text_ = std::move(result.error);
        }
    }
    if (request_pending_ || !accepted) {
        request_pending_ = false;
        startTask();
        return;
    }
    emit stateChanged();
}
