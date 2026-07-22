#include "review_model.hpp"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review model contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QVariant value(
    const ReviewModel& model,
    const int row,
    const ReviewModel::Role role
) {
    return model.data(model.index(row, 0), role);
}

struct ModelSignalCounts final {
    int resets = 0;
    int inserted = 0;
    int inserted_rows = 0;
    int removed = 0;
    int removed_rows = 0;
    int moved = 0;
    int changed = 0;
    QList<int> last_changed_roles;
};

void observe_model(ReviewModel& model, ModelSignalCounts& counts) {
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&counts]() {
        ++counts.resets;
    });
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsInserted,
        [&counts](const QModelIndex&, const int first, const int last) {
            ++counts.inserted;
            counts.inserted_rows += last - first + 1;
        }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsRemoved,
        [&counts](const QModelIndex&, const int first, const int last) {
            ++counts.removed;
            counts.removed_rows += last - first + 1;
        }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsMoved,
        [&counts](
            const QModelIndex&,
            const int,
            const int,
            const QModelIndex&,
            const int
        ) { ++counts.moved; }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::dataChanged,
        [&counts](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            ++counts.changed;
            counts.last_changed_roles = roles;
        }
    );
}

[[nodiscard]] ReviewItem keyed_item(const char* key, const char* title) {
    ReviewItem item;
    item.photo_id = QString::fromLatin1(key) + QStringLiteral("-photo");
    item.representation_id = QString::fromLatin1(key);
    item.title = QString::fromLatin1(title);
    return item;
}

void role_names_and_types_are_stable() {
    ReviewItem item;
    item.photo_id = QStringLiteral("photo-a");
    item.representation_id = QStringLiteral("representation-a");
    item.visual_handle = QStringLiteral("visual-handle-a");
    item.decision_head_sequence = 42;
    item.decision_flag = QStringLiteral("picked");
    item.decision_rating = 4;
    item.has_technical_observation = true;
    item.technical_input_width = 512;
    item.technical_input_height = 341;
    item.technical_preprocessing_version = QStringLiteral("pre-v1");
    item.technical_implementation_version = QStringLiteral("impl-v1");
    item.mean_luma = 0.42;
    item.p01_luma = 0.01;
    item.p50_luma = 0.4;
    item.p99_luma = 0.97;
    item.near_black_fraction = 0.02;
    item.near_white_fraction = 0.03;
    item.laplacian_variance = 0.004;
    item.edge_energy = 0.005;

    ReviewModel model;
    model.replace({item}, 7);

    struct ExpectedRole final {
        ReviewModel::Role role;
        const char* name;
    };
    constexpr std::array expected_roles{
        ExpectedRole{ReviewModel::VisualHandleRole, "visualHandle"},
        ExpectedRole{ReviewModel::DecisionHeadSequenceRole, "decisionHeadSequence"},
        ExpectedRole{ReviewModel::DecisionFlagRole, "decisionFlag"},
        ExpectedRole{ReviewModel::DecisionRatingRole, "decisionRating"},
        ExpectedRole{ReviewModel::ColorLabelRole, "colorLabel"},
        ExpectedRole{ReviewModel::HasTechnicalObservationRole, "hasTechnicalObservation"},
        ExpectedRole{ReviewModel::TechnicalInputWidthRole, "technicalInputWidth"},
        ExpectedRole{ReviewModel::TechnicalInputHeightRole, "technicalInputHeight"},
        ExpectedRole{
            ReviewModel::TechnicalPreprocessingVersionRole,
            "technicalPreprocessingVersion",
        },
        ExpectedRole{
            ReviewModel::TechnicalImplementationVersionRole,
            "technicalImplementationVersion",
        },
        ExpectedRole{ReviewModel::MeanLumaRole, "meanLuma"},
        ExpectedRole{ReviewModel::P01LumaRole, "p01Luma"},
        ExpectedRole{ReviewModel::P50LumaRole, "p50Luma"},
        ExpectedRole{ReviewModel::P99LumaRole, "p99Luma"},
        ExpectedRole{ReviewModel::NearBlackFractionRole, "nearBlackFraction"},
        ExpectedRole{ReviewModel::NearWhiteFractionRole, "nearWhiteFraction"},
        ExpectedRole{ReviewModel::LaplacianVarianceRole, "laplacianVariance"},
        ExpectedRole{ReviewModel::EdgeEnergyRole, "edgeEnergy"},
    };
    const auto names = model.roleNames();
    for (const auto& expected : expected_roles) {
        require(
            names.value(expected.role) == QByteArray(expected.name),
            "every technical QML role name must remain stable"
        );
    }

    require(
        value(model, 0, ReviewModel::VisualHandleRole).typeId()
                == QMetaType::QString
            && value(model, 0, ReviewModel::VisualHandleRole).toString()
                == QStringLiteral("visual-handle-a"),
        "opaque visual identity must be exposed as a string"
    );
    require(
        value(model, 0, ReviewModel::DecisionHeadSequenceRole).typeId()
                == QMetaType::ULongLong
            && value(model, 0, ReviewModel::DecisionHeadSequenceRole).toULongLong()
                == 42,
        "decision head must retain its full unsigned sequence"
    );
    require(
        value(model, 0, ReviewModel::DecisionFlagRole).typeId()
                == QMetaType::QString
            && value(model, 0, ReviewModel::DecisionFlagRole).toString()
                == QStringLiteral("picked")
            && value(model, 0, ReviewModel::DecisionRatingRole).typeId()
                == QMetaType::Int
            && value(model, 0, ReviewModel::DecisionRatingRole).toInt() == 4,
        "manual flag and rating must expose stable QML types"
    );
    require(
        value(model, 0, ReviewModel::HasTechnicalObservationRole).typeId()
            == QMetaType::Bool,
        "observation presence must be a QML boolean"
    );
    require(
        value(model, 0, ReviewModel::TechnicalInputWidthRole).typeId()
                == QMetaType::UInt
            && value(model, 0, ReviewModel::TechnicalInputHeightRole).typeId()
                == QMetaType::UInt,
        "analysis dimensions must retain unsigned integer types"
    );
    require(
        value(model, 0, ReviewModel::TechnicalPreprocessingVersionRole).typeId()
                == QMetaType::QString
            && value(model, 0, ReviewModel::TechnicalImplementationVersionRole).typeId()
                == QMetaType::QString,
        "provenance must be exposed as strings"
    );

    constexpr std::array numeric_roles{
        std::pair{ReviewModel::MeanLumaRole, 0.42},
        std::pair{ReviewModel::P01LumaRole, 0.01},
        std::pair{ReviewModel::P50LumaRole, 0.4},
        std::pair{ReviewModel::P99LumaRole, 0.97},
        std::pair{ReviewModel::NearBlackFractionRole, 0.02},
        std::pair{ReviewModel::NearWhiteFractionRole, 0.03},
        std::pair{ReviewModel::LaplacianVarianceRole, 0.004},
        std::pair{ReviewModel::EdgeEnergyRole, 0.005},
    };
    for (const auto& [role, expected] : numeric_roles) {
        const auto actual = value(model, 0, role);
        require(
            actual.typeId() == QMetaType::Double,
            "technical measurements must retain double precision"
        );
        require(
            actual.toDouble() == expected,
            "each technical role must expose its own measurement"
        );
    }
}

void color_labels_are_local_and_survive_page_refreshes() {
    ReviewItem first = keyed_item("a", "A");
    ReviewItem second = first;
    second.representation_id = QStringLiteral("a-secondary");
    ReviewItem other = keyed_item("b", "B");

    ReviewModel model;
    model.replace({first, second, other}, 1);
    require(
        model.setColorLabel(QStringLiteral("a-photo"), QStringLiteral("blue")),
        "a valid color label must update every loaded representation"
    );
    require(
        value(model, 0, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("blue")
            && value(model, 1, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("blue")
            && value(model, 2, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("none"),
        "color labels must be photo-local and never leak to another photo"
    );
    require(
        model.colorLabels().value(QStringLiteral("a-photo")).toString()
            == QStringLiteral("blue"),
        "the color-label persistence projection must retain the semantic value"
    );

    ReviewItem refreshed = first;
    refreshed.title = QStringLiteral("A refreshed");
    require(
        model.reconcileSnapshot({refreshed, other}, 1),
        "a current review refresh must reconcile after a local color label"
    );
    require(
        value(model, 0, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("blue"),
        "a catalog refresh must preserve locally persisted color labels"
    );
    require(
        !model.setColorLabel(QStringLiteral("a-photo"), QStringLiteral("orange")),
        "unsupported color labels must fail closed"
    );
}

void decision_updates_project_to_every_representation_of_a_photo() {
    ReviewItem first;
    first.photo_id = QStringLiteral("photo-a");
    first.representation_id = QStringLiteral("representation-a1");

    ReviewItem second = first;
    second.representation_id = QStringLiteral("representation-a2");

    ReviewItem other;
    other.photo_id = QStringLiteral("photo-b");
    other.representation_id = QStringLiteral("representation-b");
    other.decision_head_sequence = 2;
    other.decision_flag = QStringLiteral("rejected");
    other.decision_rating = 1;

    ReviewModel model;
    model.replace({first, second, other}, 1);
    require(
        model.updateDecision(QStringLiteral("photo-a"), 9, QStringLiteral("picked"), 5),
        "a loaded photo decision must update"
    );
    for (const int row : {0, 1}) {
        require(
            value(model, row, ReviewModel::DecisionHeadSequenceRole).toULongLong() == 9
                && value(model, row, ReviewModel::DecisionFlagRole).toString()
                    == QStringLiteral("picked")
                && value(model, row, ReviewModel::DecisionRatingRole).toInt() == 5,
            "all rows for one photo must share the materialized decision"
        );
    }
    require(
        value(model, 2, ReviewModel::DecisionHeadSequenceRole).toULongLong() == 2
            && value(model, 2, ReviewModel::DecisionFlagRole).toString()
                == QStringLiteral("rejected")
            && value(model, 2, ReviewModel::DecisionRatingRole).toInt() == 1,
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
                == QStringLiteral("7")
            && query.queryItemValue(QStringLiteral("ticket"), QUrl::FullyDecoded)
                == grid_ticket,
        "visual source must preserve the exact opaque ticket and generation"
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

void absence_and_legitimate_zero_are_distinct() {
    ReviewItem missing;
    missing.photo_id = QStringLiteral("missing");

    ReviewItem observed_zero;
    observed_zero.photo_id = QStringLiteral("observed-zero");
    observed_zero.has_technical_observation = true;

    ReviewModel model;
    model.replace({missing}, 1);
    model.append({observed_zero});

    require(model.rowCount() == 2, "append must preserve the replaced page");
    require(
        !value(model, 0, ReviewModel::HasTechnicalObservationRole).toBool(),
        "a missing observation must remain explicitly absent"
    );
    require(
        value(model, 1, ReviewModel::HasTechnicalObservationRole).toBool(),
        "an observed all-zero result must not look absent"
    );
    require(
        value(model, 0, ReviewModel::P50LumaRole).toDouble() == 0.0
            && value(model, 1, ReviewModel::P50LumaRole).toDouble() == 0.0
            && value(model, 1, ReviewModel::EdgeEnergyRole).toDouble() == 0.0,
        "zero measurements must stay numeric for both states"
    );
}

void replace_and_append_keep_their_items_intact() {
    ReviewItem first;
    first.photo_id = QStringLiteral("first");
    first.has_technical_observation = true;
    first.p50_luma = 0.25;

    ReviewItem second;
    second.photo_id = QStringLiteral("second");
    second.has_technical_observation = true;
    second.p50_luma = 0.75;

    ReviewModel model;
    model.replace({first}, 10);
    model.append({second});
    require(
        value(model, 0, ReviewModel::P50LumaRole).toDouble() == 0.25
            && value(model, 1, ReviewModel::P50LumaRole).toDouble() == 0.75,
        "append must preserve each item's own observation"
    );

    ReviewItem replacement;
    replacement.photo_id = QStringLiteral("replacement");
    replacement.has_technical_observation = false;
    model.replace({replacement}, 11);
    require(model.rowCount() == 1, "replace must remove the previous page");
    require(
        value(model, 0, ReviewModel::PhotoIdRole).toString()
                == QStringLiteral("replacement")
            && !value(model, 0, ReviewModel::HasTechnicalObservationRole).toBool(),
        "replace must expose only the new item's observation state"
    );
}

void snapshot_reconciliation_updates_visual_and_technical_roles_in_place() {
    ReviewItem initial = keyed_item("a", "initial");
    ReviewModel model;
    model.replace({initial}, 21);
    const QPersistentModelIndex stable_index(model.index(0, 0));

    ReviewItem updated = initial;
    updated.visual_handle = QStringLiteral("visual-a-ready");
    updated.visual_role = QStringLiteral("grid-preview");
    updated.visual_width = 640;
    updated.visual_height = 426;
    updated.has_visual = true;
    updated.has_technical_observation = true;
    updated.technical_input_width = 512;
    updated.technical_input_height = 341;
    updated.technical_preprocessing_version = QStringLiteral("pre-v2");
    updated.technical_implementation_version = QStringLiteral("impl-v3");
    updated.mean_luma = 0.41;
    updated.p01_luma = 0.02;
    updated.p50_luma = 0.39;
    updated.p99_luma = 0.96;
    updated.near_black_fraction = 0.03;
    updated.near_white_fraction = 0.04;
    updated.laplacian_variance = 0.007;
    updated.edge_energy = 0.009;

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(
        model.reconcileSnapshot({updated}, 21),
        "a current keyed snapshot must reconcile"
    );
    require(
        stable_index.isValid() && stable_index.row() == 0 && observed.resets == 0
            && observed.inserted == 0 && observed.removed == 0
            && observed.moved == 0 && observed.changed == 1,
        "field refresh must preserve the row and emit only dataChanged"
    );
    for (const auto role : {
             ReviewModel::VisualHandleRole,
             ReviewModel::VisualRole,
             ReviewModel::VisualErrorRole,
             ReviewModel::VisualWidthRole,
             ReviewModel::VisualHeightRole,
             ReviewModel::VisualSourceRole,
             ReviewModel::HasTechnicalObservationRole,
             ReviewModel::TechnicalInputWidthRole,
             ReviewModel::TechnicalInputHeightRole,
             ReviewModel::TechnicalPreprocessingVersionRole,
             ReviewModel::TechnicalImplementationVersionRole,
             ReviewModel::MeanLumaRole,
             ReviewModel::P01LumaRole,
             ReviewModel::P50LumaRole,
             ReviewModel::P99LumaRole,
             ReviewModel::NearBlackFractionRole,
             ReviewModel::NearWhiteFractionRole,
             ReviewModel::LaplacianVarianceRole,
             ReviewModel::EdgeEnergyRole,
         }) {
        require(
            observed.last_changed_roles.contains(role),
            "every changed visual or technical field must name its role"
        );
    }
    require(
        value(model, 0, ReviewModel::VisualHandleRole).toString()
                == QStringLiteral("visual-a-ready")
            && value(model, 0, ReviewModel::HasTechnicalObservationRole).toBool()
            && value(model, 0, ReviewModel::P50LumaRole).toDouble() == 0.39,
        "reconciled fields must be visible through the existing role contract"
    );
}

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
        value(model, 1, ReviewModel::RepresentationIdRole).toString()
                == QStringLiteral("a")
            && value(model, 2, ReviewModel::RepresentationIdRole).toString()
                == QStringLiteral("b"),
        "the model order must exactly match the sorted snapshot"
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
        observed.removed == 1 && observed.removed_rows == 2
            && observed.inserted == 1 && observed.inserted_rows == 1
            && observed.moved == 0 && observed.resets == 0,
        "obsolete and new keys must use precise remove and insert signals"
    );
    require(
        retained_index.isValid() && retained_index.row() == 0
            && value(model, 0, ReviewModel::RepresentationIdRole).toString()
                == QStringLiteral("b")
            && value(model, 1, ReviewModel::RepresentationIdRole).toString()
                == QStringLiteral("d"),
        "retained identity and final membership must survive reconciliation"
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
            && observed.removed_rows == 0 && observed.moved == 1
            && observed.changed == 1,
        "a prefix refresh must insert and reorder precisely without removing the tail"
    );
    const std::array expected_keys{"x", "c", "a", "b", "d"};
    for (int row = 0; row < static_cast<int>(expected_keys.size()); ++row) {
        require(
            value(model, row, ReviewModel::RepresentationIdRole).toString()
                == QString::fromLatin1(expected_keys.at(static_cast<std::size_t>(row))),
            "the refreshed prefix and retained tail must have deterministic order"
        );
    }
    require(
        retained_tail.isValid() && retained_tail.row() == 4
            && model.data(retained_tail, ReviewModel::RepresentationIdRole).toString()
                == QStringLiteral("d")
            && value(model, 1, ReviewModel::TitleRole).toString()
                == QStringLiteral("C updated"),
        "retained persistent identity and refreshed fields must both survive"
    );

    const QVector<QString> ids = model.representationIds();
    require(
        ids.size() == 5 && QSet<QString>(ids.cbegin(), ids.cend()).size() == 5,
        "the final presented prefix and tail must contain no duplicate stable key"
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
    require(
        !model.appendSnapshot({third, keyed_item("d", "D")}, 31)
            && !model.appendSnapshot({keyed_item("e", "E")}, 30)
            && !model.reconcilePrefixSnapshot({first, first}, 31),
        "duplicate or stale page operations must fail closed"
    );
    require(
        model.rowCount() == 3 && observed.resets == 0 && observed.inserted == 0
            && observed.removed == 0 && observed.moved == 0
            && observed.changed == 0,
        "rejected page operations must leave membership and signals untouched"
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
    require(
        !model.reconcileSnapshot({stale}, 23),
        "a stale generation must be rejected"
    );
    require(
        value(model, 0, ReviewModel::TitleRole).toString()
                == QStringLiteral("original")
            && observed.resets == 0 && observed.inserted == 0
            && observed.removed == 0 && observed.moved == 0
            && observed.changed == 0,
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
        selected.isValid() && selected.row() == 1 && observed.resets == 0
            && observed.inserted == 0 && observed.removed == 0
            && observed.moved == 0 && observed.changed == 0,
        "an identical snapshot must not disturb views or selections"
    );
}

} // namespace

int main() {
    role_names_and_types_are_stable();
    color_labels_are_local_and_survive_page_refreshes();
    decision_updates_project_to_every_representation_of_a_photo();
    visual_sources_use_encoded_tickets_and_current_generation();
    absence_and_legitimate_zero_are_distinct();
    replace_and_append_keep_their_items_intact();
    snapshot_reconciliation_updates_visual_and_technical_roles_in_place();
    snapshot_reconciliation_moves_rows_without_losing_persistent_identity();
    snapshot_reconciliation_inserts_and_removes_keyed_rows();
    prefix_reconciliation_updates_the_front_without_dropping_loaded_tail();
    prefix_and_append_reject_duplicates_without_mutating_the_model();
    snapshot_reconciliation_rejects_wrong_generation_without_mutation();
    identical_snapshot_reconciliation_is_a_signal_free_no_op();
    return EXIT_SUCCESS;
}
