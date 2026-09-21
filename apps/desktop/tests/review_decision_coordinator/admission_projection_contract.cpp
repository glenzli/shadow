#include "decision_coordinator_fixture.hpp"

#include <atomic>
#include <vector>

namespace review_decision_test {
namespace {

void flag_names_and_invalid_input_are_closed_contracts() {
    require(
        review_decision_flag_from_name(QStringLiteral("unflagged"))
                == BackendReviewDecisionFlag::Unflagged
            && review_decision_flag_from_name(QStringLiteral("picked"))
                == BackendReviewDecisionFlag::Picked
            && review_decision_flag_from_name(QStringLiteral("rejected"))
                == BackendReviewDecisionFlag::Rejected
            && !review_decision_flag_from_name(QStringLiteral("Pick")),
        "canonical flag parsing"
    );
    require(
        review_decision_flag_name(BackendReviewDecisionFlag::Picked)
            == QStringLiteral("picked"),
        "canonical flag presentation"
    );

    std::atomic<int> mutations = 0;
    std::atomic<int> resolutions = 0;
    ReviewDecisionCoordinator coordinator(
        operations(
            [&](const QString&,
                const std::uint64_t,
                const BackendReviewDecisionFlag,
                const std::uint8_t) {
                ++mutations;
                return BackendReviewDecisionMutationReceipt{};
            }
        ),
        [&](const QString&) -> std::optional<BackendReviewDecisionState> {
            ++resolutions;
            return std::nullopt;
        },
        [](const BackendReviewDecisionState&) {}
    );

    require(
        !coordinator.setFlag(
            QStringLiteral("photo-a"),
            QStringLiteral("unsupported")
        )
            && coordinator.statusText()
                == QStringLiteral("Unsupported Review flag"),
        "unsupported flag status"
    );
    require(
        !coordinator.setRating(QStringLiteral("photo-a"), 6)
            && coordinator.statusText()
                == QStringLiteral("Rating must be between 0 and 5 stars"),
        "invalid rating status"
    );
    require(
        !coordinator.setFlag(
            QStringLiteral("photo-a"),
            QStringLiteral("picked"),
            false
        ),
        "cross-workflow admission refusal"
    );
    require(
        resolutions.load() == 0 && mutations.load() == 0,
        "invalid or refused input must not resolve or mutate"
    );
    require(
        !coordinator.setRating(QStringLiteral("missing"), 4)
            && coordinator.statusText()
                == QStringLiteral(
                    "Select a loaded photo before setting a rating"
                ),
        "missing current decision status"
    );
    require(
        resolutions.load() == 1 && mutations.load() == 0,
        "missing current state must fail before mutation"
    );
}

void admitted_mutation_owns_arguments_projection_and_presentation() {
    const auto before = state(
        QStringLiteral("photo-a"),
        7,
        BackendReviewDecisionFlag::Unflagged,
        2
    );
    const auto after = state(
        QStringLiteral("photo-a"),
        9,
        BackendReviewDecisionFlag::Picked,
        2
    );
    QString received_photo_id;
    std::uint64_t received_head = 0;
    BackendReviewDecisionFlag received_flag =
        BackendReviewDecisionFlag::Unflagged;
    std::uint8_t received_rating = 0;
    std::vector<BackendReviewDecisionState> projected;
    int applied_signals = 0;
    int committed_signals = 0;
    int state_signals = 0;

    ReviewDecisionCoordinator coordinator(
        operations(
            [&](const QString& photo_id,
                const std::uint64_t expected_head,
                const BackendReviewDecisionFlag desired_flag,
                const std::uint8_t desired_rating) {
                received_photo_id = photo_id;
                received_head = expected_head;
                received_flag = desired_flag;
                received_rating = desired_rating;
                return receipt(QStringLiteral("event-pick"), before, after);
            }
        ),
        [&](const QString& photo_id) {
            require(
                photo_id == QStringLiteral("photo-a"),
                "resolver photo identity"
            );
            return std::optional<BackendReviewDecisionState>{before};
        },
        [&](const BackendReviewDecisionState& accepted) {
            projected.push_back(accepted);
        }
    );
    QObject::connect(&coordinator, &ReviewDecisionCoordinator::decisionCommitted,
                     [&](const QString& id) {
        require(id == QStringLiteral("photo-a") && !coordinator.busy(), "completion identity and settled state");
        ++committed_signals;
    });
    QObject::connect(
        &coordinator,
        &ReviewDecisionCoordinator::decisionApplied,
        [&](
            const QString& photo_id,
            const qulonglong sequence,
            const QString& flag,
            const int rating
        ) {
            ++applied_signals;
            require(
                photo_id == QStringLiteral("photo-a") && sequence == 9
                    && flag == QStringLiteral("picked") && rating == 2,
                "applied presentation signal"
            );
        }
    );
    QObject::connect(
        &coordinator,
        &ReviewDecisionCoordinator::stateChanged,
        [&]() { ++state_signals; }
    );

    require(
        coordinator.setFlag(
            QStringLiteral("photo-a"),
            QStringLiteral("picked")
        ),
        "admitted flag mutation starts"
    );
    wait_until(
        [&]() { return !coordinator.busy(); },
        "admitted flag mutation did not settle"
    );
    require(
        received_photo_id == QStringLiteral("photo-a")
            && received_head == 7
            && received_flag == BackendReviewDecisionFlag::Picked
            && received_rating == 2,
        "backend receives complete optimistic decision"
    );
    require(
        projected.size() == 1 && projected.front() == after,
        "accepted authoritative state is projected once"
    );
    require(
        coordinator.canUndo() && applied_signals == 1 && state_signals >= 2
            && committed_signals == 1,
        "successful mutation exposes undo and state changes"
    );
    require(
        coordinator.statusText()
            == QStringLiteral("Manual decision recorded · sequence 9"),
        "successful mutation status"
    );
}

} // namespace

void run_admission_projection_contracts() {
    flag_names_and_invalid_input_are_closed_contracts();
    admitted_mutation_owns_arguments_projection_and_presentation();
}

} // namespace review_decision_test
