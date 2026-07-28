#include "comparison_backend_fixture.hpp"

namespace review_comparison_test {
namespace {

void record_and_forget_serialize_the_complete_async_lifecycle() {
    FakeComparisonBackend backend;
    QSemaphore record_started;
    QSemaphore release_record;
    backend.record_started = &record_started;
    backend.release_record = &release_record;
    auto coordinator = make_coordinator(backend);
    int recorded = 0;
    int forgotten = 0;
    QObject::connect(
        &coordinator,
        &ReviewComparisonCoordinator::recorded,
        [&]() { ++recorded; }
    );
    QObject::connect(
        &coordinator,
        &ReviewComparisonCoordinator::forgotten,
        [&]() { ++forgotten; }
    );

    require(
        coordinator.record(QStringLiteral("presentation-a"), 0),
        "the first evidence write must start"
    );
    require(
        record_started.tryAcquire(1, 1'000),
        "record worker did not start"
    );
    require(coordinator.busy(), "record worker must own visible busy state");
    require(
        !coordinator.record(QStringLiteral("presentation-b"), 1),
        "a second write must not overlap the active record"
    );
    coordinator.cancel(QStringLiteral("presentation-a"));
    require(
        backend.cancel_calls == 0,
        "presentation cancellation must not race an evidence write"
    );
    release_record.release();
    wait_until(
        [&]() { return !coordinator.busy() && recorded == 1; },
        "record receipt did not reach the unique success terminal"
    );
    require(
        coordinator.activeCount() == 1 && coordinator.canForget(),
        "a valid receipt must publish one session-local active event"
    );
    require(
        coordinator.statusText().contains(QStringLiteral("sequence 11")),
        "record terminal status must carry the durable sequence"
    );

    QSemaphore forget_started;
    QSemaphore release_forget;
    backend.forget_started = &forget_started;
    backend.release_forget = &release_forget;
    require(coordinator.forgetLast(), "latest evidence must begin forgetting");
    require(
        forget_started.tryAcquire(1, 1'000),
        "forget worker did not start"
    );
    require(
        coordinator.busy() && !coordinator.canForget(),
        "forget worker must serialize another undo"
    );
    release_forget.release();
    wait_until(
        [&]() { return !coordinator.busy() && forgotten == 1; },
        "forget receipt did not reach the unique success terminal"
    );
    require(
        backend.forgotten_event_id == QStringLiteral("event-1"),
        "forget must target the newest exact event"
    );
    require(
        coordinator.activeCount() == 0 && !coordinator.canForget(),
        "successful forget must remove only its session-local target"
    );
}

} // namespace

void run_evidence_lifecycle_contracts() {
    record_and_forget_serialize_the_complete_async_lifecycle();
}

} // namespace review_comparison_test
