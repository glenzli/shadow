#include "comparison_backend_fixture.hpp"

#include <memory>
#include <thread>

namespace review_comparison_test {
namespace {

void worker_errors_release_pending_state_and_destruction_waits() {
    FakeComparisonBackend failing_backend;
    failing_backend.throw_record = true;
    auto failing = make_coordinator(failing_backend);
    require(
        failing.record(QStringLiteral("presentation"), 0),
        "failing record must start"
    );
    wait_until(
        [&]() { return !failing.busy(); },
        "record error did not release pending state"
    );
    require(
        failing.activeCount() == 0
            && failing.statusText().contains(
                QStringLiteral("sentinel record failure")
            ),
        "record error must retain no evidence and expose its diagnostic"
    );

    FakeComparisonBackend blocking_backend;
    QSemaphore started;
    QSemaphore release;
    blocking_backend.record_started = &started;
    blocking_backend.release_record = &release;
    std::atomic<bool> released = false;
    auto blocking = std::make_unique<ReviewComparisonCoordinator>(
        blocking_backend.operations(),
        [](const QString& ticket) { return ticket; }
    );
    require(
        blocking->record(QStringLiteral("presentation"), 0),
        "destruction-wait record must start"
    );
    require(started.tryAcquire(1, 1'000), "destruction-wait worker did not start");
    std::thread releaser([&]() {
        QThread::msleep(20);
        released.store(true);
        release.release();
    });
    blocking.reset();
    releaser.join();
    require(
        released.load() && blocking_backend.record_calls.load() == 1,
        "coordinator destruction must wait for its in-flight worker"
    );
}

} // namespace

void run_failure_lifetime_contracts() {
    worker_errors_release_pending_state_and_destruction_waits();
}

} // namespace review_comparison_test
