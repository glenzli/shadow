#include "decision_coordinator_fixture.hpp"

#include <atomic>
#include <stdexcept>
#include <vector>

namespace review_decision_test {
namespace {

void verified_inverse_preserves_append_only_history() {
    const auto initial = state(
        QStringLiteral("photo-a"),
        0,
        BackendReviewDecisionFlag::Unflagged,
        0
    );
    const auto picked = state(
        QStringLiteral("photo-a"),
        4,
        BackendReviewDecisionFlag::Picked,
        0
    );
    const auto restored = state(
        QStringLiteral("photo-a"),
        5,
        BackendReviewDecisionFlag::Unflagged,
        0
    );
    BackendReviewDecisionState visible = initial;
    std::atomic<int> calls = 0;
    int undone = 0;
    int committed = 0;

    ReviewDecisionCoordinator coordinator(
        operations(
            [&](const QString&,
                const std::uint64_t expected_head,
                const BackendReviewDecisionFlag desired_flag,
                const std::uint8_t desired_rating) {
                const int call = ++calls;
                if (call == 1) {
                    require(
                        expected_head == 0
                            && desired_flag
                                == BackendReviewDecisionFlag::Picked
                            && desired_rating == 0,
                        "forward mutation arguments"
                    );
                    return receipt(
                        QStringLiteral("event-pick"),
                        initial,
                        picked
                    );
                }
                require(
                    expected_head == 4
                        && desired_flag
                            == BackendReviewDecisionFlag::Unflagged
                        && desired_rating == 0,
                    "inverse mutation arguments"
                );
                return receipt(
                    QStringLiteral("event-undo"),
                    picked,
                    restored
                );
            }
        ),
        [&](const QString&) {
            return std::optional<BackendReviewDecisionState>{visible};
        },
        [&](const BackendReviewDecisionState& accepted) {
            visible = accepted;
        }
    );
    QObject::connect(&coordinator, &ReviewDecisionCoordinator::decisionCommitted,
                     [&]() { ++committed; });
    QObject::connect(
        &coordinator,
        &ReviewDecisionCoordinator::undone,
        [&]() { ++undone; }
    );

    require(
        coordinator.setFlag(
            QStringLiteral("photo-a"),
            QStringLiteral("picked")
        ),
        "forward decision starts"
    );
    wait_until(
        [&]() { return !coordinator.busy(); },
        "forward decision did not settle"
    );
    require(coordinator.canUndo(), "forward decision becomes undoable");
    require(coordinator.undo(), "inverse decision starts");
    wait_until(
        [&]() { return !coordinator.busy(); },
        "inverse decision did not settle"
    );
    require(
        visible == restored && calls.load() == 2 && undone == 1 && committed == 1
            && !coordinator.canUndo(),
        "verified inverse projects the new authoritative head"
    );
    require(
        coordinator.statusText()
            == QStringLiteral(
                "Inverse decision appended · sequence 5 · history retained"
            ),
        "inverse decision status"
    );
}

void failure_refreshes_authoritative_state_and_invalid_receipts_fail_closed() {
    const auto before = state(
        QStringLiteral("photo-failure"),
        1,
        BackendReviewDecisionFlag::Unflagged,
        0
    );
    const auto refreshed = state(
        QStringLiteral("photo-failure"),
        3,
        BackendReviewDecisionFlag::Rejected,
        1
    );
    std::vector<BackendReviewDecisionState> projected;
    ReviewDecisionCoordinator failing(
        operations(
            [](const QString&,
               const std::uint64_t,
               const BackendReviewDecisionFlag,
               const std::uint8_t)
                -> BackendReviewDecisionMutationReceipt {
                throw std::runtime_error("write conflict");
            },
            [&](const QString&) { return refreshed; }
        ),
        [&](const QString&) {
            return std::optional<BackendReviewDecisionState>{before};
        },
        [&](const BackendReviewDecisionState& accepted) {
            projected.push_back(accepted);
        }
    );
    int completions = 0;
    QObject::connect(&failing, &ReviewDecisionCoordinator::decisionCommitted,
                     [&]() { ++completions; });
    require(
        failing.setRating(QStringLiteral("photo-failure"), 4),
        "failing decision starts"
    );
    wait_until(
        [&]() { return !failing.busy(); },
        "failing decision did not settle"
    );
    require(
        projected.size() == 1 && projected.front() == refreshed && completions == 0,
        "write failure projects refreshed authoritative state"
    );
    require(
        failing.statusText()
            == QStringLiteral(
                "Decision write failed · authoritative state refreshed · "
                "write conflict"
            ),
        "write failure status"
    );

    int invalid_projection_count = 0;
    ReviewDecisionCoordinator invalid(
        operations(
            [&](const QString&,
                const std::uint64_t,
                const BackendReviewDecisionFlag,
                const std::uint8_t) {
                return BackendReviewDecisionMutationReceipt{
                    .event_id = {},
                    .sequence = 2,
                    .occurred_at_ms = 1,
                    .before = before,
                    .after = state(
                        QStringLiteral("photo-failure"),
                        2,
                        BackendReviewDecisionFlag::Picked,
                        0
                    ),
                };
            }
        ),
        [&](const QString&) {
            return std::optional<BackendReviewDecisionState>{before};
        },
        [&](const BackendReviewDecisionState&) {
            ++invalid_projection_count;
        }
    );
    QObject::connect(&invalid, &ReviewDecisionCoordinator::decisionCommitted,
                     [&]() { ++completions; });
    require(
        invalid.setFlag(
            QStringLiteral("photo-failure"),
            QStringLiteral("picked")
        ),
        "invalid-receipt mutation starts"
    );
    wait_until(
        [&]() { return !invalid.busy(); },
        "invalid-receipt mutation did not settle"
    );
    require(
        invalid_projection_count == 0 && !invalid.canUndo() && completions == 0
            && invalid.statusText()
                == QStringLiteral(
                    "Decision receipt was invalid; local state retained"
                ),
        "invalid receipt cannot project or enter undo history"
    );
}

} // namespace

void run_failure_undo_contracts() {
    verified_inverse_preserves_append_only_history();
    failure_refreshes_authoritative_state_and_invalid_receipts_fail_closed();
}

} // namespace review_decision_test
