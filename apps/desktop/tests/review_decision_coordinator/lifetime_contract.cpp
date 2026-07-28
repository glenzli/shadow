#include "decision_coordinator_fixture.hpp"

#include <QSemaphore>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace review_decision_test {
namespace {

void incomplete_dependencies_are_rejected() {
    bool rejected_operations = false;
    try {
        ReviewDecisionCoordinator coordinator(
            {},
            [](const QString&) {
                return std::optional<BackendReviewDecisionState>{};
            },
            [](const BackendReviewDecisionState&) {}
        );
    } catch (const std::invalid_argument&) {
        rejected_operations = true;
    }
    require(rejected_operations, "incomplete backend operations are rejected");

    bool rejected_projection = false;
    try {
        ReviewDecisionCoordinator coordinator(
            operations(
                [](const QString&,
                   const std::uint64_t,
                   const BackendReviewDecisionFlag,
                   const std::uint8_t) {
                    return BackendReviewDecisionMutationReceipt{};
                }
            ),
            {},
            {}
        );
    } catch (const std::invalid_argument&) {
        rejected_projection = true;
    }
    require(
        rejected_projection,
        "missing resolver and projection are rejected"
    );
}

void destruction_waits_for_the_active_mutation() {
    const auto before = state(
        QStringLiteral("photo-lifetime"),
        0,
        BackendReviewDecisionFlag::Unflagged,
        0
    );
    const auto after = state(
        QStringLiteral("photo-lifetime"),
        1,
        BackendReviewDecisionFlag::Picked,
        0
    );
    QSemaphore started;
    QSemaphore release;
    std::atomic<bool> finished = false;
    auto coordinator = std::make_unique<ReviewDecisionCoordinator>(
        operations(
            [&](const QString&,
                const std::uint64_t,
                const BackendReviewDecisionFlag,
                const std::uint8_t) {
                started.release();
                release.acquire();
                finished = true;
                return receipt(
                    QStringLiteral("event-lifetime"),
                    before,
                    after
                );
            }
        ),
        [&](const QString&) {
            return std::optional<BackendReviewDecisionState>{before};
        },
        [](const BackendReviewDecisionState&) {}
    );
    require(
        coordinator->setFlag(
            QStringLiteral("photo-lifetime"),
            QStringLiteral("picked")
        ),
        "lifetime mutation starts"
    );
    require(
        started.tryAcquire(1, 3'000),
        "lifetime worker did not start"
    );

    std::thread releaser([&]() {
        QThread::msleep(30);
        release.release();
    });
    coordinator.reset();
    releaser.join();
    require(
        finished.load(),
        "coordinator destruction abandoned its active mutation"
    );
}

} // namespace

void run_lifetime_contracts() {
    incomplete_dependencies_are_rejected();
    destruction_waits_for_the_active_mutation();
}

} // namespace review_decision_test
