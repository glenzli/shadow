#include "library_organization_fixture.hpp"

#include <chrono>
#include <thread>

namespace review_library_organization_test {

void run_failure_lifetime_contracts() {
    {
        auto state = std::make_shared<OrganizationBackendState>();
        state->current.insert(QStringLiteral("photo"), {});
        state->fail = true;
        ReviewLibraryOrganizationCoordinator coordinator(operations(state));
        coordinator.setLiked(QStringLiteral("photo"), true, true);
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "failed organization mutation terminates"
        );
        require(
            coordinator.statusMessage().translated().contains(
                QStringLiteral("organization write failed")
            ),
            "backend failure preserves its diagnostic"
        );
    }

    {
        auto state = std::make_shared<OrganizationBackendState>();
        state->current.insert(QStringLiteral("photo"), {});
        state->wrong_receipt = true;
        ReviewLibraryOrganizationCoordinator coordinator(operations(state));
        coordinator.setLiked(QStringLiteral("photo"), true, true);
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "invalid receipt reaches a terminal state"
        );
        require(
            coordinator.statusMessage().translated()
                == QStringLiteral("Library state receipt was invalid")
                && state->projections.isEmpty(),
            "wrong-photo receipt fails closed before model projection"
        );
    }

    {
        auto state = std::make_shared<OrganizationBackendState>();
        state->current.insert(QStringLiteral("photo"), {});
        state->block = true;
        auto coordinator =
            std::make_unique<ReviewLibraryOrganizationCoordinator>(
                operations(state)
            );
        coordinator->setLiked(QStringLiteral("photo"), true, true);
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() {
                return state->entered;
            });
        }
        std::thread releaser([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            {
                std::lock_guard lock(state->mutex);
                state->release = true;
            }
            state->condition.notify_all();
        });
        QElapsedTimer timer;
        timer.start();
        coordinator.reset();
        releaser.join();
        require(
            timer.elapsed() >= 25,
            "destruction waits for the active organization mutation"
        );
    }
}

} // namespace review_library_organization_test
