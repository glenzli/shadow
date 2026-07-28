#include "review_model_fixture.hpp"

namespace review_model_test {

void library_state_is_catalog_authoritative_and_photo_scoped() {
    ReviewItem first = keyed_item("a", "A");
    ReviewItem other = keyed_item("b", "B");

    ReviewModel model;
    model.replace({first, other}, 1);
    require(
        model.updateLibraryState(
            QStringLiteral("a-photo"),
            true,
            QStringLiteral("blue"),
            17
        ),
        "a Catalog Library-state receipt must update its loaded photo"
    );
    require(
        value(model, 0, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("blue")
            && value(model, 0, ReviewModel::LikedRole).toBool()
            && value(model, 0, ReviewModel::LibraryStateUpdatedAtMsRole).toLongLong()
                == 17
            && value(model, 1, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("none"),
        "Library state must be photo-local and never leak to another photo"
    );
    const auto projected = model.libraryStateFor(QStringLiteral("a-photo"));
    require(projected && projected->liked
                && projected->color_label == QStringLiteral("blue")
                && projected->updated_at_ms == 17,
        "controller lookup must expose the Catalog-authoritative Library state");

    ReviewItem refreshed = first;
    refreshed.title = QStringLiteral("A refreshed");
    refreshed.liked = false;
    refreshed.color_label = QStringLiteral("red");
    refreshed.library_state_updated_at_ms = 18;
    require(
        model.reconcileSnapshot({refreshed, other}, 1),
        "a current Library page must reconcile after a durable state update"
    );
    require(
        value(model, 0, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("red")
            && !value(model, 0, ReviewModel::LikedRole).toBool()
            && value(model, 0, ReviewModel::LibraryStateUpdatedAtMsRole).toLongLong()
                == 18,
        "a refreshed Catalog row must replace stale local state rather than retain settings"
    );
    require(
        !model.updateLibraryState(
            QStringLiteral("a-photo"),
            true,
            QStringLiteral("orange"),
            19
        ),
        "unsupported Catalog color labels must fail closed"
    );
}

void decision_updates_project_to_the_single_photo_row() {
    ReviewItem first;
    first.photo_id = QStringLiteral("photo-a");
    first.representation_id = QStringLiteral("representation-a1");

    ReviewItem other;
    other.photo_id = QStringLiteral("photo-b");
    other.representation_id = QStringLiteral("representation-b");
    other.decision_head_sequence = 2;
    other.decision_flag = QStringLiteral("rejected");
    other.decision_rating = 1;

    ReviewModel model;
    model.replace({first, other}, 1);
    require(
        model.updateDecision(QStringLiteral("photo-a"), 9, QStringLiteral("picked"), 5),
        "a loaded photo decision must update"
    );
    require(
        value(model, 0, ReviewModel::DecisionHeadSequenceRole).toULongLong() == 9
            && value(model, 0, ReviewModel::DecisionFlagRole).toString()
                == QStringLiteral("picked")
            && value(model, 0, ReviewModel::DecisionRatingRole).toInt() == 5,
        "the one visible row for a logical photo must project its decision"
    );
    require(
        value(model, 1, ReviewModel::DecisionHeadSequenceRole).toULongLong() == 2
            && value(model, 1, ReviewModel::DecisionFlagRole).toString()
                == QStringLiteral("rejected")
            && value(model, 1, ReviewModel::DecisionRatingRole).toInt() == 1,
        "updating one photo must not change another photo"
    );
    const auto projected = model.decisionFor(QStringLiteral("photo-a"));
    require(
        projected && projected->head_sequence == 9
            && projected->flag == QStringLiteral("picked") && projected->rating == 5,
        "controller lookup must read the projected full state"
    );
    require(
        !model.updateDecision(QStringLiteral("missing"), 10, QStringLiteral("picked"), 1)
            && !model.updateDecision(
                QStringLiteral("photo-a"),
                10,
                QStringLiteral("future-flag"),
                1
            )
            && !model.updateDecision(
                QStringLiteral("photo-a"),
                10,
                QStringLiteral("picked"),
                6
            ),
        "unknown rows and invalid desired states must fail closed"
    );
}

} // namespace review_model_test
