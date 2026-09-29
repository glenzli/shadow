#include "review_model_fixture.hpp"

namespace review_model_test {

void snapshot_reconciliation_moves_rows_without_losing_persistent_identity() {
    const ReviewItem first = keyed_item("a", "A");
    const ReviewItem second = keyed_item("b", "B");
    const ReviewItem third = keyed_item("c", "C");
    ReviewModel model;
    model.replace({first, second, third}, 22);
    const QPersistentModelIndex selected(model.index(2, 0));

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(
        model.reconcileSnapshot({third, first, second}, 22),
        "a reordered current snapshot must reconcile"
    );
    require(
        observed.moved == 1 && observed.resets == 0 && observed.inserted == 0
            && observed.removed == 0 && observed.changed == 0,
        "a pure reorder must emit one row move and no reset"
    );
    require(
        selected.isValid() && selected.row() == 0
            && model.data(selected, ReviewModel::RepresentationIdRole).toString()
                   == QStringLiteral("c"),
        "the selected representation must follow its moved row"
    );
    require(
        value(model, 1, ReviewModel::RepresentationIdRole).toString() == QStringLiteral("a")
            && value(model, 2, ReviewModel::RepresentationIdRole).toString() == QStringLiteral("b"),
        "the model order must exactly match the sorted snapshot"
    );
    const bool decision_updated =
        model.updateDecision(QStringLiteral("c-photo"), 7, QStringLiteral("picked"), 5);
    const auto moved_decision = model.decisionFor(QStringLiteral("c-photo"));
    require(
        decision_updated && value(model, 0, ReviewModel::DecisionRatingRole).toInt() == 5
            && moved_decision && moved_decision->rating == 5,
        "photo-id lookup must follow a row after reconciliation moves it"
    );
}

void snapshot_reconciliation_inserts_and_removes_keyed_rows() {
    const ReviewItem removed_first = keyed_item("a", "A");
    const ReviewItem removed_second = keyed_item("c", "C");
    const ReviewItem retained = keyed_item("b", "B");
    const ReviewItem inserted = keyed_item("d", "D");
    ReviewModel model;
    model.replace({removed_first, removed_second, retained}, 23);
    const QPersistentModelIndex retained_index(model.index(2, 0));

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(
        model.reconcileSnapshot({retained, inserted}, 23),
        "membership changes in a current snapshot must reconcile"
    );
    require(
        observed.removed == 1 && observed.removed_rows == 2 && observed.inserted == 1
            && observed.inserted_rows == 1 && observed.moved == 0 && observed.resets == 0,
        "obsolete and new keys must use precise remove and insert signals"
    );
    require(
        retained_index.isValid() && retained_index.row() == 0
            && value(model, 0, ReviewModel::RepresentationIdRole).toString() == QStringLiteral("b")
            && value(model, 1, ReviewModel::RepresentationIdRole).toString() == QStringLiteral("d"),
        "retained identity and final membership must survive reconciliation"
    );
    const bool library_state_updated =
        model.updateLibraryState(QStringLiteral("d-photo"), true, QStringLiteral("blue"), 9);
    const auto inserted_state = model.libraryStateFor(QStringLiteral("d-photo"));
    require(
        !model.decisionFor(QStringLiteral("a-photo")) && library_state_updated && inserted_state
            && inserted_state->liked && value(model, 1, ReviewModel::LikedRole).toBool(),
        "photo-id lookup must reflect removed and inserted rows"
    );
}

void prefix_reconciliation_updates_the_front_without_dropping_loaded_tail() {
    const ReviewItem first = keyed_item("a", "A");
    const ReviewItem second = keyed_item("b", "B");
    ReviewItem refreshed = keyed_item("c", "C updated");
    const ReviewItem tail = keyed_item("d", "D");
    const ReviewItem inserted = keyed_item("x", "X");
    ReviewModel model;
    model.replace({first, second, keyed_item("c", "C"), tail}, 30);
    const QPersistentModelIndex retained_tail(model.index(3, 0));

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(
        model.reconcilePrefixSnapshot({inserted, refreshed, first}, 30),
        "a current keyed prefix must reconcile"
    );
    require(
        model.rowCount() == 5 && observed.resets == 0 && observed.inserted_rows == 1
            && observed.removed_rows == 0 && observed.moved == 1 && observed.changed == 1,
        "a prefix refresh must insert and reorder precisely without removing the tail"
    );
    const std::array expected_keys{"x", "c", "a", "b", "d"};
    for (int row = 0; row < static_cast<int>(expected_keys.size()); ++row) {
        require(
            value(model, row, ReviewModel::PhotoIdRole).toString()
                == QString::fromLatin1(expected_keys.at(static_cast<std::size_t>(row)))
                       + QStringLiteral("-photo"),
            "the refreshed prefix and retained tail must have deterministic order"
        );
    }
    require(
        retained_tail.isValid() && retained_tail.row() == 4
            && model.data(retained_tail, ReviewModel::PhotoIdRole).toString()
                   == QStringLiteral("d-photo")
            && value(model, 1, ReviewModel::TitleRole).toString() == QStringLiteral("C updated"),
        "retained persistent identity and refreshed fields must both survive"
    );

    QVector<QString> ids;
    ids.reserve(model.rowCount());
    for (int row = 0; row < model.rowCount(); ++row) {
        ids.push_back(value(model, row, ReviewModel::PhotoIdRole).toString());
    }
    require(
        ids.size() == 5 && QSet<QString>(ids.cbegin(), ids.cend()).size() == 5,
        "the final presented prefix and tail must contain no duplicate photo id"
    );
}

void prefix_and_append_reject_duplicates_without_mutating_the_model() {
    const ReviewItem first = keyed_item("a", "A");
    const ReviewItem second = keyed_item("b", "B");
    const ReviewItem third = keyed_item("c", "C");
    ReviewModel model;
    model.replace({first, second}, 31);

    require(
        model.appendSnapshot({third}, 31) && model.rowCount() == 3,
        "a disjoint current-generation page must append"
    );
    ModelSignalCounts observed;
    observe_model(model, observed);
    ReviewItem relinked_duplicate = first;
    relinked_duplicate.representation_id = QStringLiteral("a-relinked");
    require(
        !model.appendSnapshot({third, keyed_item("d", "D")}, 31)
            && !model.appendSnapshot({keyed_item("e", "E")}, 30)
            && !model.reconcilePrefixSnapshot({first, relinked_duplicate}, 31),
        "duplicate or stale page operations must fail closed"
    );
    require(
        model.rowCount() == 3 && observed.resets == 0 && observed.inserted == 0
            && observed.removed == 0 && observed.moved == 0 && observed.changed == 0,
        "rejected page operations must leave membership and signals untouched"
    );
    require(
        model.reconcileSnapshot({second, third}, 31) && model.appendSnapshot({first}, 31)
            && model.rowCount() == 3,
        "a removed photo id must become available to a later valid page"
    );
    model.append({first});
    require(
        !model.appendSnapshot({keyed_item("f", "F")}, 31),
        "an invalid direct append must not make later page validation trust stale membership"
    );
    model.replace({second, third}, 32);
    require(
        model.appendSnapshot({first}, 32) && model.rowCount() == 3,
        "replacement must rebuild the page-membership index"
    );
}

void snapshot_reconciliation_rejects_wrong_generation_without_mutation() {
    const ReviewItem initial = keyed_item("a", "original");
    ReviewItem stale = initial;
    stale.title = QStringLiteral("stale");
    ReviewModel model;
    model.replace({initial}, 24);

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(!model.reconcileSnapshot({stale}, 23), "a stale generation must be rejected");
    require(
        value(model, 0, ReviewModel::TitleRole).toString() == QStringLiteral("original")
            && observed.resets == 0 && observed.inserted == 0 && observed.removed == 0
            && observed.moved == 0 && observed.changed == 0,
        "generation rejection must be silent and leave the model untouched"
    );
}

void identical_snapshot_reconciliation_is_a_signal_free_no_op() {
    const ReviewItem first = keyed_item("a", "A");
    const ReviewItem second = keyed_item("b", "B");
    ReviewModel model;
    model.replace({first, second}, 25);
    const QPersistentModelIndex selected(model.index(1, 0));

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(
        model.reconcileSnapshot({first, second}, 25),
        "an identical current snapshot must be accepted"
    );
    require(
        selected.isValid() && selected.row() == 1 && observed.resets == 0 && observed.inserted == 0
            && observed.removed == 0 && observed.moved == 0 && observed.changed == 0,
        "an identical snapshot must not disturb views or selections"
    );
}

} // namespace review_model_test
