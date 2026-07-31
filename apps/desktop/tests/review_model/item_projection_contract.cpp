#include "review_model_fixture.hpp"

namespace review_model_test {

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
        value(model, 0, ReviewModel::PhotoIdRole).toString() == QStringLiteral("replacement")
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
    require(model.reconcileSnapshot({updated}, 21), "a current keyed snapshot must reconcile");
    require(
        stable_index.isValid() && stable_index.row() == 0 && observed.resets == 0
            && observed.inserted == 0 && observed.removed == 0 && observed.moved == 0
            && observed.changed == 1,
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

void photo_identity_survives_representation_relink() {
    ReviewItem original = keyed_item("a", "Original");
    original.location_id = QStringLiteral("location-old");
    original.source_path = QStringLiteral("/old/location/a.raw");
    original.source_available = false;
    original.visual_handle = QStringLiteral("visual-old");
    ReviewModel model;
    model.replace({original}, 22);
    const QPersistentModelIndex selected(model.index(0, 0));

    ReviewItem relinked = original;
    relinked.representation_id = QStringLiteral("a-relinked-representation");
    relinked.location_id = QStringLiteral("location-new");
    relinked.source_path = QStringLiteral("/new/location/a.raw");
    relinked.source_available = true;
    relinked.visual_handle = QStringLiteral("visual-relinked");

    ModelSignalCounts observed;
    observe_model(model, observed);
    require(
        model.reconcileSnapshot({relinked}, 22),
        "a relinked representation must reconcile under the original photo id"
    );
    require(
        model.rowCount() == 1 && selected.isValid() && selected.row() == 0 && observed.resets == 0
            && observed.inserted == 0 && observed.removed == 0 && observed.moved == 0
            && observed.changed == 1,
        "a relink must update the existing logical photo instead of duplicating it"
    );
    require(
        value(model, 0, ReviewModel::PhotoIdRole).toString() == QStringLiteral("a-photo")
            && value(model, 0, ReviewModel::RepresentationIdRole).toString()
                   == QStringLiteral("a-relinked-representation")
            && value(model, 0, ReviewModel::SourcePathRole).toString()
                   == QStringLiteral("/new/location/a.raw")
            && value(model, 0, ReviewModel::LocationIdRole).toString()
                   == QStringLiteral("location-new")
            && value(model, 0, ReviewModel::SourceAvailableRole).toBool()
            && value(model, 0, ReviewModel::VisualHandleRole).toString()
                   == QStringLiteral("visual-relinked")
            && observed.last_changed_roles.contains(ReviewModel::RepresentationIdRole)
            && observed.last_changed_roles.contains(ReviewModel::SourcePathRole)
            && observed.last_changed_roles.contains(ReviewModel::LocationIdRole)
            && observed.last_changed_roles.contains(ReviewModel::SourceAvailableRole)
            && observed.last_changed_roles.contains(ReviewModel::VisualHandleRole),
        "photo-first reconciliation must project the replacement representation fields"
    );
}

} // namespace review_model_test
