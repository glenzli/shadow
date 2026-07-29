#include "library_facet_fixture.hpp"

#include <chrono>
#include <thread>

namespace review_library_facet_test {

void run_coalescing_failure_lifetime_contracts() {
    {
        auto state = std::make_shared<FacetBackendState>();
        state->block_first = true;
        ReviewLibraryFacetCoordinator coordinator(operations(state));
        BackendLibraryPhotoFilter first;
        first.camera_key = QStringLiteral("first");
        coordinator.refresh(first, 1);
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() { return state->first_entered; });
        }
        BackendLibraryPhotoFilter latest;
        latest.camera_key = QStringLiteral("latest");
        coordinator.refresh(latest, 2);
        coordinator.refresh(latest, 2);
        {
            std::lock_guard lock(state->mutex);
            state->release_first = true;
        }
        state->condition.notify_all();
        wait_until(
            [&coordinator, &state]() { return !coordinator.busy() && call_count(state) == 12; },
            "stale batch is replaced by one coalesced latest-generation batch"
        );
        require(
            coordinator.cameras().front().toMap().value(QStringLiteral("key"))
                    == QStringLiteral("latest-camera")
                && call_count(state) == 12,
            "only the latest generation reaches the visible projection"
        );
    }

    {
        auto state = std::make_shared<FacetBackendState>();
        state->fail = true;
        ReviewLibraryFacetCoordinator coordinator(operations(state));
        coordinator.refresh({}, 1);
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "failed facet batch reaches a terminal state"
        );
        require(
            coordinator.globalStatusMessage().translated().contains(
                QStringLiteral("facet query failed")
            ),
            "failure publishes localized global status with diagnostics"
        );
    }

    {
        auto state = std::make_shared<FacetBackendState>();
        state->block_first = true;
        auto coordinator = std::make_unique<ReviewLibraryFacetCoordinator>(operations(state));
        coordinator->refresh({}, 1);
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
        require(timer.elapsed() >= 25, "destruction waits for the active three-facet batch");
    }
}

} // namespace review_library_facet_test
