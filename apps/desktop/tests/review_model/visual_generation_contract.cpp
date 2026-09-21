#include "review_model_fixture.hpp"

namespace review_model_test {

void visual_sources_use_encoded_tickets_and_current_generation() {
    const QString grid_ticket = QStringLiteral("ticket:/a b?x=1&literal=%2F#tail");
    ReviewItem item;
    item.photo_id = QStringLiteral("photo-a");
    item.representation_id = QStringLiteral("representation-should-not-be-used");
    item.visual_handle = grid_ticket;
    item.has_visual = true;

    ReviewModel model;
    model.replace({item}, 7);

    const QString source_text =
        value(model, 0, ReviewModel::VisualSourceRole).toString();
    const QUrl source(source_text);
    const QUrlQuery query(source);
    require(
        source.scheme() == QStringLiteral("image")
            && source.host() == QStringLiteral("shadow")
            && source.path() == QStringLiteral("/visual"),
        "visual source must target the single ticket-based image resource"
    );
    require(
        query.queryItemValue(QStringLiteral("generation"), QUrl::FullyDecoded)
                == QStringLiteral("0")
            && query.queryItemValue(QStringLiteral("lifetime"), QUrl::FullyDecoded)
                   == QStringLiteral("grid")
            && query.queryItemValue(QStringLiteral("ticket"), QUrl::FullyDecoded) == grid_ticket,
        "grid source must preserve the exact opaque ticket and use a stable cache identity"
    );
    require(
        !source_text.contains(QStringLiteral("representation-should-not-be-used")),
        "representation id must not remain an image-provider identity"
    );

    const QString comparison_ticket = QStringLiteral("compare/left?nonce=a&b=c");
    const QUrl comparison_source(model.visualSourceFor(comparison_ticket));
    const QUrlQuery comparison_query(comparison_source);
    require(
        comparison_source.path() == QStringLiteral("/visual")
            && comparison_query.queryItemValue(
                   QStringLiteral("generation"),
                   QUrl::FullyDecoded
               )
                == QStringLiteral("7")
            && comparison_query.queryItemValue(
                   QStringLiteral("lifetime"),
                   QUrl::FullyDecoded
               )
                == QStringLiteral("comparison")
            && comparison_query.queryItemValue(
                   QStringLiteral("ticket"),
                   QUrl::FullyDecoded
               )
                == comparison_ticket,
        "controller-created comparison sources must share the exact URL contract"
    );

    ReviewItem replacement;
    replacement.visual_handle = QStringLiteral("not-displayable");
    replacement.has_visual = false;
    model.replace({replacement}, 8);
    require(
        value(model, 0, ReviewModel::VisualSourceRole).toString().isEmpty(),
        "an unavailable visual must not issue a provider request"
    );
    require(
        model.visualSourceFor(QString{}).isEmpty(),
        "an empty request ticket must not produce a provider URL"
    );
    require(
        QUrlQuery(QUrl(model.visualSourceFor(QStringLiteral("fresh"))))
                .queryItemValue(QStringLiteral("generation"), QUrl::FullyDecoded)
            == QStringLiteral("8"),
        "comparison sources must always use the current model generation"
    );
}

void generation_advance_reuses_immutable_grid_visuals() {
    ReviewItem first = keyed_item("a", "A");
    first.visual_handle = QStringLiteral("visual-a");
    first.has_visual = true;
    ReviewItem second = keyed_item("b", "B");
    second.visual_handle = QStringLiteral("visual-b");
    second.has_visual = true;

    ReviewModel model;
    model.replace({first, second}, 2);
    const QString old_source =
        value(model, 0, ReviewModel::VisualSourceRole).toString();
    ModelSignalCounts observed;
    observe_model(model, observed);

    model.setGeneration(3);

    const QString current_source =
        value(model, 0, ReviewModel::VisualSourceRole).toString();
    require(
        model.isGenerationCurrent(3) && current_source == old_source,
        "query navigation must reuse the same signed immutable grid URL"
    );
    require(
        observed.resets == 0 && observed.inserted == 0 && observed.removed == 0
            && observed.moved == 0 && observed.changed == 0,
        "generation advance must not reload retained grid images"
    );
    require(
        QUrlQuery(QUrl(model.visualSourceFor(QStringLiteral("comparison"))))
                .queryItemValue(QStringLiteral("generation"), QUrl::FullyDecoded)
            == QStringLiteral("3"),
        "comparison requests remain bound to the current generation"
    );
    model.setGeneration(3);
    require(observed.changed == 0, "repeating a generation is signal-free");

    first.visual_handle = QStringLiteral("new-artifact-a");
    require(model.reconcileSnapshot({first, second}, 3), "changed artifacts reconcile");
    require(
        value(model, 0, ReviewModel::VisualSourceRole).toString() != old_source
            && observed.changed == 1
            && observed.last_changed_roles.contains(ReviewModel::VisualSourceRole),
        "an actual artifact change must still invalidate the grid image"
    );
}

} // namespace review_model_test
