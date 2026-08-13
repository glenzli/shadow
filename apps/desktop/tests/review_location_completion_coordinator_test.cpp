#include "review_location_completion_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Location completion coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void waitUntil(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

BackendReviewItem photo(
    const QString& id,
    const std::int64_t captured_at,
    const bool has_coordinates = false,
    const std::int32_t latitude_e7 = 0,
    const std::int32_t longitude_e7 = 0,
    const QString& place_name = {}
) {
    return {
        .photo_id = id,
        .title = id,
        .source_path = QStringLiteral("/photos/") + id + QStringLiteral(".raw"),
        .captured_at_unix_seconds = captured_at,
        .has_coordinates = has_coordinates,
        .latitude_e7 = latitude_e7,
        .longitude_e7 = longitude_e7,
        .place_name = place_name,
    };
}

ReviewLocationCompletionCoordinator::Operations operations() {
    return {
        .page = [](const BackendLibraryPhotoFilter& filter,
                   const BackendLibraryPhotoOrder order,
                   const BackendLibraryPhotoCursor& cursor,
                   const std::uint32_t limit) {
            require(
                order == BackendLibraryPhotoOrder::CaptureTimeAscending && limit == 256,
                "events read bounded capture-time pages"
            );
            require(
                filter.has_capture_start && filter.capture_start_unix_seconds == 10
                    && filter.has_capture_end && filter.capture_end_unix_seconds == 40'000,
                "the requested time range reaches the Catalog unchanged"
            );
            if (cursor.photo_id.isEmpty()) {
                return BackendLibraryPhotoPage{
                    .items = {
                        photo(QStringLiteral("anchor"), 100, true, 312'304'000, 1'212'473'000, QStringLiteral("Shanghai")),
                        photo(QStringLiteral("missing-a"), 180),
                        photo(QStringLiteral("missing-b"), 4'000),
                        photo(QStringLiteral("far-anchor"), 20'000, true, 399'043'390, 1'164'211'220, QStringLiteral("Beijing")),
                        photo(QStringLiteral("missing-c"), 20'040),
                    },
                    .has_more = true,
                    .next_cursor = {.photo_id = QStringLiteral("second")},
                };
            }
            return BackendLibraryPhotoPage{
                .items = {photo(QStringLiteral("missing-d"), 20'080)},
                .has_more = false,
            };
        },
        .anchors = [](const std::int64_t start, const std::int64_t end) {
            require(start == 10 && end == 40'000, "the event range also bounds reference anchors");
            return QVector<BackendLocationReferenceAnchor>{
                {
                    .library_id = QStringLiteral("phone"),
                    .captured_at_unix_seconds = 4'100,
                    .latitude_e7 = 312'304'050,
                    .longitude_e7 = 1'212'473'020,
                },
                {
                    .library_id = QStringLiteral("phone"),
                    .captured_at_unix_seconds = 80'000,
                    .latitude_e7 = 399'043'390,
                    .longitude_e7 = 1'164'211'220,
                },
            };
        },
    };
}

void runContract() {
    bool rejected_missing_operation = false;
    try {
        ReviewLocationCompletionCoordinator coordinator({});
    } catch (const std::invalid_argument&) {
        rejected_missing_operation = true;
    }
    require(rejected_missing_operation, "construction requires all bounded read operations");

    ReviewLocationCompletionCoordinator coordinator(operations());
    coordinator.request({}, 10, 40'000);
    require(coordinator.busy(), "a location-completion request publishes busy");
    waitUntil(
        [&coordinator]() { return !coordinator.busy(); },
        "the event query reaches a terminal state"
    );
    require(coordinator.errorText().isEmpty(), "a successful query has no error");
    require(!coordinator.truncated(), "two bounded pages do not report truncation");
    require(coordinator.groups().size() == 2, "time gaps form two event groups");

    const QVariantMap first = coordinator.groups().at(0).toMap();
    require(
        first.value(QStringLiteral("targetCount")).toInt() == 2
            && first.value(QStringLiteral("hasSuggestion")).toBool()
            && first.value(QStringLiteral("anchorCount")).toInt() == 2
            && first.value(QStringLiteral("placeName")).toString() == QStringLiteral("Shanghai")
            && first.value(QStringLiteral("targets")).toList().size() == 2,
        "time-near reference anchors join the event without becoming mutation targets"
    );
    const QVariantMap second = coordinator.groups().at(1).toMap();
    require(
        second.value(QStringLiteral("targetCount")).toInt() == 2
            && second.value(QStringLiteral("hasSuggestion")).toBool()
            && second.value(QStringLiteral("placeName")).toString() == QStringLiteral("Beijing"),
        "the next event preserves its independent time-local anchor"
    );

    coordinator.invalidate();
    require(
        coordinator.groups().isEmpty() && !coordinator.truncated()
            && coordinator.errorText().isEmpty(),
        "metadata changes can discard stale suggestions without mutation"
    );
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    runContract();
    return EXIT_SUCCESS;
}
