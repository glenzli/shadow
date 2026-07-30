#include "review_library_map_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Library map coordinator contract failed: " << message << '\n';
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

struct MapCall final {
    BackendLibraryPhotoFilter filter;
    BackendLibraryMapViewport viewport;
    BackendLibraryMapGrid grid;
};

struct MapBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<MapCall> calls;
    bool block_first = false;
    bool first_entered = false;
    bool release_first = false;
    bool fail = false;
};

ReviewLibraryMapCoordinator::Operations operations(const std::shared_ptr<MapBackendState>& state) {
    return {
        .snapshot = [state](
                        const BackendLibraryPhotoFilter& filter,
                        const BackendLibraryMapViewport& viewport,
                        const BackendLibraryMapGrid& grid
                    ) {
            std::unique_lock lock(state->mutex);
            state->calls.push_back({
                .filter = filter,
                .viewport = viewport,
                .grid = grid,
            });
            if (state->block_first && state->calls.size() == 1) {
                state->first_entered = true;
                state->condition.notify_all();
                state->condition.wait(lock, [state]() { return state->release_first; });
            }
            if (state->fail) {
                throw std::runtime_error("map query failed");
            }
            return BackendLibraryMapSnapshot{
                .clusters =
                    {
                        {
                            .cell_x = 2,
                            .cell_y = 3,
                            .latitude_e7 = 312'300'000,
                            .longitude_e7 = 1'214'700'000,
                            .photo_count = 1,
                            .photo_id = filter.camera_key + QStringLiteral("-photo"),
                            .representation_id = QStringLiteral("representation"),
                            .title = filter.camera_key,
                            .source_path = QStringLiteral("/photos/source.nef"),
                        },
                        {
                            .cell_x = 4,
                            .cell_y = 5,
                            .latitude_e7 = 306'700'000,
                            .longitude_e7 = 1'040'600'000,
                            .photo_count = 2,
                        },
                    },
                .photo_count = 3,
            };
        },
    };
}

int callCount(const std::shared_ptr<MapBackendState>& state) {
    std::lock_guard lock(state->mutex);
    return static_cast<int>(state->calls.size());
}

void runProjectionContract() {
    bool rejected_missing_operation = false;
    try {
        ReviewLibraryMapCoordinator coordinator({});
    } catch (const std::invalid_argument&) {
        rejected_missing_operation = true;
    }
    require(
        rejected_missing_operation,
        "construction rejects a missing Catalog snapshot operation"
    );

    auto state = std::make_shared<MapBackendState>();
    ReviewLibraryMapCoordinator coordinator(operations(state));
    BackendLibraryPhotoFilter filter;
    filter.camera_key = QStringLiteral("nikon-z9");
    filter.album_id = QStringLiteral("favorites");
    filter.has_liked = true;
    filter.liked = true;
    const BackendLibraryMapViewport viewport{
        .south_latitude_e7 = -100,
        .west_longitude_e7 = 1'700'000'000,
        .north_latitude_e7 = 200,
        .east_longitude_e7 = -1'700'000'000,
    };
    const BackendLibraryMapGrid grid{.columns = 12, .rows = 8};

    coordinator.request(filter, viewport, grid);
    require(coordinator.busy(), "a map request publishes its busy state");
    waitUntil(
        [&coordinator]() { return !coordinator.busy(); },
        "the map request reaches a terminal state"
    );
    require(!coordinator.failed(), "a successful map request clears failure");
    require(
        coordinator.photoCount() == 3 && coordinator.clusters().size() == 2,
        "the complete bounded snapshot is published atomically"
    );

    const QVariantMap singleton = coordinator.clusters().front().toMap();
    require(
        singleton.value(QStringLiteral("cellX")).toUInt() == 2
            && singleton.value(QStringLiteral("cellY")).toUInt() == 3
            && singleton.value(QStringLiteral("latitude")).toDouble() == 31.23
            && singleton.value(QStringLiteral("longitude")).toDouble() == 121.47
            && singleton.value(QStringLiteral("photoCount")).toULongLong() == 1
            && singleton.value(QStringLiteral("photoId")).toString()
                   == QStringLiteral("nikon-z9-photo")
            && singleton.value(QStringLiteral("representationId")).toString()
                   == QStringLiteral("representation")
            && singleton.value(QStringLiteral("title")).toString() == QStringLiteral("nikon-z9")
            && singleton.value(QStringLiteral("sourcePath")).toString()
                   == QStringLiteral("/photos/source.nef"),
        "the QML projection preserves singleton identity and display fields"
    );

    {
        std::lock_guard lock(state->mutex);
        require(
            state->calls.size() == 1
                && state->calls.front().filter.album_id == QStringLiteral("favorites")
                && state->calls.front().filter.has_liked && state->calls.front().filter.liked
                && state->calls.front().viewport.west_longitude_e7 == 1'700'000'000
                && state->calls.front().viewport.east_longitude_e7 == -1'700'000'000
                && state->calls.front().grid.columns == 12 && state->calls.front().grid.rows == 8,
            "the current filter, antimeridian viewport, and screen grid reach the backend unchanged"
        );
    }
}

void runCoalescingFailureAndLifetimeContracts() {
    {
        auto state = std::make_shared<MapBackendState>();
        state->block_first = true;
        ReviewLibraryMapCoordinator coordinator(operations(state));
        BackendLibraryPhotoFilter first;
        first.camera_key = QStringLiteral("first");
        coordinator.request(first, {}, {.columns = 1, .rows = 1});
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() { return state->first_entered; });
        }

        BackendLibraryPhotoFilter latest;
        latest.camera_key = QStringLiteral("latest");
        coordinator.request(latest, {}, {.columns = 2, .rows = 2});
        coordinator.request(latest, {}, {.columns = 3, .rows = 3});
        {
            std::lock_guard lock(state->mutex);
            state->release_first = true;
        }
        state->condition.notify_all();
        waitUntil(
            [&coordinator, &state]() { return !coordinator.busy() && callCount(state) == 2; },
            "stale work is replaced by one coalesced latest request"
        );
        require(
            coordinator.clusters().front().toMap().value(QStringLiteral("title")).toString()
                == QStringLiteral("latest"),
            "only the latest request reaches the visible projection"
        );
        {
            std::lock_guard lock(state->mutex);
            require(
                state->calls.back().grid.columns == 3 && state->calls.back().grid.rows == 3,
                "coalescing retains the latest screen grid"
            );
        }
    }

    {
        auto state = std::make_shared<MapBackendState>();
        ReviewLibraryMapCoordinator coordinator(operations(state));
        BackendLibraryPhotoFilter filter;
        filter.camera_key = QStringLiteral("stable");
        coordinator.request(filter, {}, {.columns = 1, .rows = 1});
        waitUntil(
            [&coordinator]() { return !coordinator.busy(); },
            "the baseline snapshot completes"
        );
        state->fail = true;
        coordinator.request(filter, {}, {.columns = 1, .rows = 1});
        waitUntil(
            [&coordinator]() { return !coordinator.busy(); },
            "the failed refresh reaches a terminal state"
        );
        require(
            coordinator.failed() && coordinator.photoCount() == 3
                && coordinator.clusters().size() == 2,
            "failure is explicit while preserving the last accepted snapshot"
        );
    }

    {
        auto state = std::make_shared<MapBackendState>();
        state->block_first = true;
        auto coordinator = std::make_unique<ReviewLibraryMapCoordinator>(operations(state));
        coordinator->request({}, {}, {.columns = 1, .rows = 1});
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() { return state->first_entered; });
        }
        std::thread releaser([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            {
                std::lock_guard lock(state->mutex);
                state->release_first = true;
            }
            state->condition.notify_all();
        });
        QElapsedTimer timer;
        timer.start();
        coordinator.reset();
        releaser.join();
        require(timer.elapsed() >= 25, "destruction waits for the active local Catalog query");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    runProjectionContract();
    runCoalescingFailureAndLifetimeContracts();
    return EXIT_SUCCESS;
}
