#include "review_decision_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review decision session contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] BackendReviewDecisionState state(
    const char* photo_id,
    const std::uint64_t sequence,
    const BackendReviewDecisionFlag flag,
    const std::uint8_t rating
) {
    return {
        .photo_id = QString::fromLatin1(photo_id),
        .head_sequence = sequence,
        .flag = flag,
        .rating = rating,
    };
}

[[nodiscard]] BackendReviewDecisionMutationReceipt receipt(
    const char* event_id,
    const BackendReviewDecisionState& before,
    const BackendReviewDecisionState& after
) {
    return {
        .event_id = QString::fromLatin1(event_id),
        .sequence = after.head_sequence,
        .occurred_at_ms = 1,
        .before = before,
        .after = after,
    };
}

void successful_commands_form_a_lifo_inverse_stack() {
    ReviewDecisionSession session;
    const auto a0 = state("photo-a", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto a1 = state("photo-a", 5, BackendReviewDecisionFlag::Picked, 0);
    const auto b0 = state("photo-b", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto b1 = state("photo-b", 8, BackendReviewDecisionFlag::Unflagged, 4);

    const auto first = session.beginSet(a0, BackendReviewDecisionFlag::Picked, 0);
    require(first && !first->is_undo, "the first manual decision must begin");
    require(!session.beginSet(b0, BackendReviewDecisionFlag::Unflagged, 4),
            "a second mutation must not start while one is pending");
    require(session.complete(receipt("event-a", a0, a1)), "first receipt must complete");

    const auto second = session.beginSet(b0, BackendReviewDecisionFlag::Unflagged, 4);
    require(second.has_value(), "a second photo command must begin after completion");
    require(session.complete(receipt("event-b", b0, b1)), "second receipt must complete");
    require(session.undoDepth() == 2 && session.canUndo(), "both commands must be undoable");

    const auto undo_b = session.beginUndo();
    require(
        undo_b && undo_b->photo_id == QStringLiteral("photo-b")
            && undo_b->expected_head_sequence == 8
            && undo_b->desired_flag == BackendReviewDecisionFlag::Unflagged
            && undo_b->desired_rating == 0,
        "undo must target the latest command with a complete prior state"
    );
    const auto b2 = state("photo-b", 11, BackendReviewDecisionFlag::Unflagged, 0);
    require(session.complete(receipt("undo-b", b1, b2)), "inverse receipt must complete");
    require(session.undoDepth() == 1 && session.canUndo(), "older photo command remains");

    const auto undo_a = session.beginUndo();
    require(
        undo_a && undo_a->photo_id == QStringLiteral("photo-a"),
        "LIFO undo must return to the older photo command"
    );
    const auto a2 = state("photo-a", 12, BackendReviewDecisionFlag::Unflagged, 0);
    require(session.complete(receipt("undo-a", a1, a2)), "older inverse completes");
    require(session.undoDepth() == 0 && !session.canUndo(), "all commands are consumed");
}

void mismatched_receipt_never_pops_the_command() {
    ReviewDecisionSession session;
    const auto before = state("photo-a", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto after = state("photo-a", 3, BackendReviewDecisionFlag::Picked, 2);
    require(
        session.beginSet(before, BackendReviewDecisionFlag::Picked, 2).has_value(),
        "set begins"
    );
    require(session.complete(receipt("set", before, after)), "set completes");
    require(session.beginUndo().has_value(), "undo begins");

    auto wrong_after = state("photo-a", 4, BackendReviewDecisionFlag::Rejected, 0);
    require(
        !session.complete(receipt("wrong-undo", after, wrong_after)),
        "a receipt with the wrong desired state must fail closed"
    );
    require(
        session.undoDepth() == 1 && session.canUndo(),
        "a malformed inverse receipt must not pop or invalidate the command"
    );
}

void same_photo_commands_remain_chained_across_inverse_heads() {
    ReviewDecisionSession session;
    const auto initial = state("photo-a", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto picked = state("photo-a", 1, BackendReviewDecisionFlag::Picked, 0);
    const auto rated = state("photo-a", 2, BackendReviewDecisionFlag::Picked, 4);

    require(
        session.beginSet(initial, BackendReviewDecisionFlag::Picked, 0).has_value()
            && session.complete(receipt("pick", initial, picked)),
        "same-photo pick completes"
    );
    require(
        session.beginSet(picked, BackendReviewDecisionFlag::Picked, 4).has_value()
            && session.complete(receipt("rate", picked, rated)),
        "same-photo rating completes"
    );

    const auto undo_rating = session.beginUndo();
    require(
        undo_rating && undo_rating->expected_head_sequence == 2
            && undo_rating->desired_flag == BackendReviewDecisionFlag::Picked
            && undo_rating->desired_rating == 0,
        "first inverse targets the rating command"
    );
    const auto restored_pick = state("photo-a", 3, BackendReviewDecisionFlag::Picked, 0);
    require(
        session.complete(receipt("undo-rate", rated, restored_pick)),
        "rating inverse completes"
    );
    require(
        session.undoDepth() == 1 && session.canUndo(),
        "verified inverse head must rebind the causal pick command"
    );

    const auto undo_pick = session.beginUndo();
    require(
        undo_pick && undo_pick->expected_head_sequence == 3
            && undo_pick->desired_flag == BackendReviewDecisionFlag::Unflagged
            && undo_pick->desired_rating == 0,
        "second inverse must use the new trusted head"
    );
    const auto restored_initial = state(
        "photo-a",
        4,
        BackendReviewDecisionFlag::Unflagged,
        0
    );
    require(
        session.complete(receipt("undo-pick", restored_pick, restored_initial)),
        "pick inverse completes"
    );
    require(session.undoDepth() == 0 && !session.canUndo(), "same-photo chain is consumed");
}

void interleaved_photos_keep_each_causal_undo_head() {
    ReviewDecisionSession session;
    const auto a0 = state("photo-a", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto a1 = state("photo-a", 1, BackendReviewDecisionFlag::Picked, 0);
    const auto b0 = state("photo-b", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto b1 = state("photo-b", 2, BackendReviewDecisionFlag::Unflagged, 3);
    const auto a2 = state("photo-a", 3, BackendReviewDecisionFlag::Picked, 5);

    require(
        session.beginSet(a0, BackendReviewDecisionFlag::Picked, 0).has_value()
            && session.complete(receipt("a-pick", a0, a1))
            && session.beginSet(b0, BackendReviewDecisionFlag::Unflagged, 3).has_value()
            && session.complete(receipt("b-rate", b0, b1))
            && session.beginSet(a1, BackendReviewDecisionFlag::Picked, 5).has_value()
            && session.complete(receipt("a-rate", a1, a2)),
        "interleaved commands complete"
    );

    require(session.beginUndo().has_value(), "latest A command is undoable");
    const auto a3 = state("photo-a", 4, BackendReviewDecisionFlag::Picked, 0);
    require(session.complete(receipt("undo-a-rate", a2, a3)), "A rating inverse completes");

    const auto undo_b = session.beginUndo();
    require(
        undo_b && undo_b->photo_id == QStringLiteral("photo-b")
            && undo_b->expected_head_sequence == 2,
        "global LIFO next targets B"
    );
    const auto b2 = state("photo-b", 5, BackendReviewDecisionFlag::Unflagged, 0);
    require(session.complete(receipt("undo-b-rate", b1, b2)), "B inverse completes");

    const auto undo_a = session.beginUndo();
    require(
        undo_a && undo_a->photo_id == QStringLiteral("photo-a")
            && undo_a->expected_head_sequence == 4,
        "older A command retains the inverse event's rebased head"
    );
}

void authoritative_conflicts_disable_undo_without_losing_history() {
    ReviewDecisionSession session;
    const auto before = state("photo-a", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto after = state("photo-a", 3, BackendReviewDecisionFlag::Picked, 2);
    require(
        session.beginSet(before, BackendReviewDecisionFlag::Picked, 2).has_value()
            && session.complete(receipt("set", before, after)),
        "local command completes"
    );

    session.reconcile(state("photo-a", 6, BackendReviewDecisionFlag::Rejected, 2));
    require(
        session.undoDepth() == 1 && !session.canUndo(),
        "an external state change must disable overwrite-style undo but retain history"
    );
    session.reconcile(state("photo-a", 7, BackendReviewDecisionFlag::Picked, 2));
    require(
        !session.canUndo(),
        "returning to the same values at another head must not hide external history"
    );
    session.reconcile(after);
    require(session.canUndo(), "an unchanged authoritative revision remains retryable");
    const auto retry = session.beginUndo();
    require(retry && retry->expected_head_sequence == 3, "retry keeps the exact known head");
    session.fail();
    require(session.canUndo(), "a transient write failure must leave matching undo retryable");
}

void a_local_inverse_never_bridges_an_external_same_photo_head() {
    ReviewDecisionSession session;
    const auto initial = state("photo-a", 0, BackendReviewDecisionFlag::Unflagged, 0);
    const auto local_pick = state("photo-a", 1, BackendReviewDecisionFlag::Picked, 0);
    require(
        session.beginSet(initial, BackendReviewDecisionFlag::Picked, 0).has_value()
            && session.complete(receipt("local-pick", initial, local_pick)),
        "first local command completes"
    );

    const auto external_reject = state("photo-a", 2, BackendReviewDecisionFlag::Rejected, 0);
    session.reconcile(external_reject);
    const auto local_rating = state("photo-a", 3, BackendReviewDecisionFlag::Rejected, 4);
    require(
        session.beginSet(external_reject, BackendReviewDecisionFlag::Rejected, 4).has_value()
            && session.complete(receipt("local-rate", external_reject, local_rating)),
        "a later local command may begin from the reconciled external head"
    );
    require(session.beginUndo().has_value(), "later local command is undoable");
    const auto restored_external = state(
        "photo-a",
        4,
        BackendReviewDecisionFlag::Rejected,
        0
    );
    require(
        session.complete(receipt("undo-local-rate", local_rating, restored_external)),
        "local inverse restores the external values at a new head"
    );
    require(
        session.undoDepth() == 1 && !session.canUndo(),
        "the inverse must not rebind across the unmatched external head"
    );
}

} // namespace

int main() {
    successful_commands_form_a_lifo_inverse_stack();
    mismatched_receipt_never_pops_the_command();
    same_photo_commands_remain_chained_across_inverse_heads();
    interleaved_photos_keep_each_causal_undo_head();
    authoritative_conflicts_disable_undo_without_losing_history();
    a_local_inverse_never_bridges_an_external_same_photo_head();
    return EXIT_SUCCESS;
}
