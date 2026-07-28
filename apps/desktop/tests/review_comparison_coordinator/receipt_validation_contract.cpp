#include "comparison_backend_fixture.hpp"

namespace review_comparison_test {
namespace {

void outcome_mapping_and_malformed_receipts_fail_closed() {
    FakeComparisonBackend backend;
    auto coordinator = make_coordinator(backend);
    const std::vector<BackendPairwiseOutcome> expected{
        BackendPairwiseOutcome::LeftPreferred,
        BackendPairwiseOutcome::RightPreferred,
        BackendPairwiseOutcome::KeepBoth,
        BackendPairwiseOutcome::KeepNeither,
        BackendPairwiseOutcome::CannotCompare,
    };

    require(
        !coordinator.record(QStringLiteral("presentation"), -1, false)
            && coordinator.statusText()
                == QStringLiteral("Comparison outcome is not supported"),
        "input validation must precede a cross-workflow admission rejection"
    );
    require(
        !coordinator.record({}, 0, false)
            && coordinator.statusText()
                == QStringLiteral("Prepare and verify the comparison first"),
        "missing presentation validation must precede admission rejection"
    );
    require(
        backend.record_calls.load() == 0,
        "rejected admission must never schedule a worker"
    );
    require(
        !coordinator.record(QStringLiteral("presentation"), -1),
        "negative outcome must be rejected"
    );
    require(
        !coordinator.record(QStringLiteral("presentation"), 5),
        "out-of-range outcome must be rejected"
    );
    for (int outcome = 0; outcome < static_cast<int>(expected.size()); ++outcome) {
        require(
            coordinator.record(QStringLiteral("presentation"), outcome),
            "supported outcome must start"
        );
        wait_until(
            [&]() { return !coordinator.busy(); },
            "supported outcome did not finish"
        );
    }
    {
        std::scoped_lock lock(backend.record_mutex);
        require(
            backend.recorded_outcomes == expected,
            "integer outcomes must preserve the public QML mapping"
        );
    }

    backend.forget_target_override = QStringLiteral("different-event");
    require(coordinator.forgetLast(), "mismatched forget must still run");
    wait_until(
        [&]() { return !coordinator.busy(); },
        "mismatched forget did not finish"
    );
    require(
        coordinator.activeCount() == static_cast<int>(expected.size())
            && coordinator.canForget(),
        "mismatched forget receipt must retain every active event"
    );
    require(
        coordinator.statusText()
            == QStringLiteral("Forget receipt was invalid; evidence retained"),
        "mismatched forget must publish the invalid-receipt terminal"
    );

    FakeComparisonBackend malformed_backend;
    malformed_backend.record_event_override = QStringLiteral("   ");
    auto malformed = make_coordinator(malformed_backend);
    require(
        malformed.record(QStringLiteral("presentation"), 0),
        "malformed receipt scenario must start"
    );
    wait_until(
        [&]() { return !malformed.busy(); },
        "malformed record receipt did not finish"
    );
    require(
        malformed.activeCount() == 0 && !malformed.canForget(),
        "malformed record receipt must not invent evidence"
    );
}

} // namespace

void run_receipt_validation_contracts() {
    outcome_mapping_and_malformed_receipts_fail_closed();
}

} // namespace review_comparison_test
