#include "library_query_fixture.hpp"

#include <chrono>
#include <thread>

namespace review_library_query_test {

void run_coalescing_failure_lifetime_contracts() {
    {
        auto state = std::make_shared<QueryBackendState>();
        ReviewModel model;
        ReviewLibraryQueryCoordinator coordinator(operations(state), model);
        BackendLibraryPhotoFilter typing;
        typing.camera_key = QStringLiteral("typing");
        coordinator.scheduleReset(typing, BackendLibraryPhotoOrder::CaptureTimeDescending);
        BackendLibraryPhotoFilter person;
        person.camera_key = QStringLiteral("person");
        coordinator.requestReset(person, BackendLibraryPhotoOrder::CaptureTimeDescending);
        require(
            coordinator.pageRunning() && coordinator.generation() == 2,
            "explicit navigation begins synchronously, bypassing the filter debounce"
        );
        wait_until(
            [&coordinator]() { return !coordinator.refreshing(); },
            "the immediate navigation page completes"
        );
        QThread::msleep(150);
        QCoreApplication::processEvents();
        require(
            page_call_count(state) == 1,
            "navigation cancels the pending filter timer rather than issuing a second query"
        );
        require(
            model.data(model.index(0, 0), ReviewModel::PhotoIdRole).toString() == "person",
            "only the final navigation filter is queried"
        );
    }
    {
        auto state = std::make_shared<QueryBackendState>();
        state->block_first_page = true;
        ReviewModel model;
        ReviewLibraryQueryCoordinator coordinator(operations(state), model);
        QStringList presented;
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsInserted,
            &model,
            [&model, &presented](const QModelIndex&, int first, int last) {
                for (int row = first; row <= last; ++row)
                    presented.push_back(
                        model.data(model.index(row, 0), ReviewModel::PhotoIdRole).toString()
                    );
            }
        );
        BackendLibraryPhotoFilter first;
        first.camera_key = QStringLiteral("first");
        coordinator.requestReset(first, BackendLibraryPhotoOrder::CaptureTimeDescending);
        wait_for_first_page(state);
        {
            std::lock_guard lock(state->mutex);
            require(
                state->count_calls.isEmpty(),
                "the initial page reaches the serialized Catalog before its aggregate count"
            );
        }

        BackendLibraryPhotoFilter latest;
        latest.camera_key = QStringLiteral("latest");
        coordinator.scheduleReset(latest, BackendLibraryPhotoOrder::FileNameDescending);
        coordinator.scheduleReset(latest, BackendLibraryPhotoOrder::FileNameDescending);
        QThread::msleep(150);
        QCoreApplication::processEvents();
        release_first_page(state);
        wait_until(
            [&coordinator, &model, &state]() {
                return !coordinator.refreshing() && model.rowCount() == 1
                       && model.data(model.index(0, 0), ReviewModel::PhotoIdRole).toString()
                              == QStringLiteral("latest")
                       && page_call_count(state) == 2;
            },
            "one coalesced latest reset supersedes the completed older page"
        );
        require(
            presented == QStringList{QStringLiteral("latest")},
            "a superseded page must never flash old photos or instantiate their thumbnails"
        );
        require(
            coordinator.generation() == 3,
            "each executed reset advances exactly one model generation"
        );
        {
            std::lock_guard lock(state->mutex);
            require(
                state->count_calls == QVector<QString>{QStringLiteral("latest")},
                "only the presented reset schedules its secondary aggregate count"
            );
        }
    }

    {
        auto state = std::make_shared<QueryBackendState>();
        state->invalid_cursor = true;
        ReviewModel model;
        ReviewLibraryQueryCoordinator coordinator(operations(state), model);
        BackendLibraryPhotoFilter filter;
        filter.camera_key = QStringLiteral("paginate");
        coordinator.requestReset(filter, BackendLibraryPhotoOrder::CaptureTimeDescending);
        wait_until(
            [&coordinator]() { return !coordinator.refreshing(); },
            "invalid continuation reaches a terminal query state"
        );
        require(
            coordinator.statusMessage().translated().contains(
                QStringLiteral("invalid continuation cursor")
            ),
            "invalid continuation is rejected before model publication"
        );
    }

    {
        auto state = std::make_shared<QueryBackendState>();
        state->fail_count = true;
        ReviewModel model;
        ReviewLibraryQueryCoordinator coordinator(operations(state), model);
        coordinator.requestReset({}, BackendLibraryPhotoOrder::CaptureTimeDescending);
        wait_until(
            [&coordinator]() {
                return coordinator.statusMessage().translated().contains(
                    QStringLiteral("count failed")
                );
            },
            "count failure publishes localized diagnostics"
        );
    }

    {
        auto state = std::make_shared<QueryBackendState>();
        state->block_first_page = true;
        ReviewModel model;
        auto coordinator =
            std::make_unique<ReviewLibraryQueryCoordinator>(operations(state), model);
        coordinator->requestReset({}, BackendLibraryPhotoOrder::CaptureTimeDescending);
        wait_for_first_page(state);
        std::thread releaser([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            release_first_page(state);
        });
        QElapsedTimer timer;
        timer.start();
        coordinator.reset();
        releaser.join();
        require(timer.elapsed() >= 25, "destruction waits for the active page worker");
    }
}

} // namespace review_library_query_test
