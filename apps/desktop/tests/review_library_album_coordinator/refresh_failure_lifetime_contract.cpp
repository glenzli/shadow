#include "library_album_fixture.hpp"

#include <chrono>
#include <thread>

namespace review_library_album_test {

void run_refresh_failure_lifetime_contracts() {
    {
        auto state = std::make_shared<AlbumBackendState>();
        state->block_list = true;
        ReviewLibraryAlbumCoordinator coordinator(operations(state));
        coordinator.refresh();
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() {
                return state->list_entered;
            });
        }
        coordinator.refresh();
        coordinator.refresh();
        {
            std::lock_guard lock(state->mutex);
            state->block_list = false;
            state->release_list = true;
        }
        state->condition.notify_all();
        wait_until(
            [&coordinator, &state]() {
                return !coordinator.busy() && list_calls(state) == 2;
            },
            "concurrent refresh requests coalesce into one follow-up"
        );
        require(
            list_calls(state) == 2,
            "refresh coalescing does not start redundant workers"
        );
    }

    {
        auto state = std::make_shared<AlbumBackendState>();
        state->fail_create = true;
        ReviewLibraryAlbumCoordinator coordinator(operations(state));
        coordinator.createManual(QStringLiteral("Fails"));
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "failed mutation reaches a terminal state"
        );
        require(
            coordinator.statusChannel()
                == ReviewLibraryAlbumCoordinator::StatusChannel::Global
                && coordinator.statusMessage().translated().contains(
                    QStringLiteral("create failed")
                ),
            "mutation failure preserves diagnostics on the global status channel"
        );
    }

    {
        auto state = std::make_shared<AlbumBackendState>();
        state->block_list = true;
        auto coordinator = std::make_unique<ReviewLibraryAlbumCoordinator>(
            operations(state)
        );
        coordinator->refresh();
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() {
                return state->list_entered;
            });
        }
        std::thread releaser([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            {
                std::lock_guard lock(state->mutex);
                state->release_list = true;
            }
            state->condition.notify_all();
        });
        QElapsedTimer timer;
        timer.start();
        coordinator.reset();
        releaser.join();
        require(
            timer.elapsed() >= 25,
            "destruction waits for the active album worker"
        );
    }
}

} // namespace review_library_album_test
