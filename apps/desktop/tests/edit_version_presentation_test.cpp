#include "edit_version_presentation.hpp"
#include "edit_version_model.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit version presentation contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void root_has_a_semantic_summary() {
    BackendEditVersion version;
    version.is_root = true;
    require(
        EditVersionPresentation::changeSummary(version) == QStringLiteral("Initial version"),
        "a root must not look like an empty diff"
    );
    require(
        EditVersionPresentation::parentSummary(version) == QStringLiteral("Root"),
        "a root must describe its lineage"
    );
}

void basic_keys_are_localizable_labels() {
    BackendEditVersion version;
    version.changed_basic_parameters = {
        QStringLiteral("exposure_stops"),
        QStringLiteral("saturation_factor"),
    };
    version.changed_basic_parameter_count = 2;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Exposure · Saturation"),
        "known parameter keys must become readable labels"
    );
}

void every_basic_key_has_a_stable_label() {
    BackendEditVersion version;
    version.changed_basic_parameters = {
        QStringLiteral("exposure_stops"),
        QStringLiteral("contrast_factor"),
        QStringLiteral("red_channel_gain"),
        QStringLiteral("green_channel_gain"),
        QStringLiteral("blue_channel_gain"),
        QStringLiteral("saturation_factor"),
    };
    version.changed_basic_parameter_count = 6;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral(
                "Exposure · Contrast · Red gain · Green gain · Blue gain · Saturation"
            ),
        "every renderer-backed basic parameter needs a stable display label"
    );
}

void unknown_keys_are_never_exposed_or_dropped() {
    BackendEditVersion version;
    version.changed_basic_parameters = {
        QStringLiteral("future_local_contrast"),
        QStringLiteral("future_dehaze"),
    };
    version.changed_basic_parameter_count = 2;
    const auto summary = EditVersionPresentation::changeSummary(version);
    require(
        summary == QStringLiteral("Other adjustments"),
        "multiple unknown keys must remain visible through a fallback"
    );
    require(
        !summary.contains(QStringLiteral("future_")),
        "raw localization keys must never leak into the UI"
    );
}

void structural_and_basic_changes_compose() {
    BackendEditVersion version;
    version.changed_basic_parameters = {QStringLiteral("contrast_factor")};
    version.changed_basic_parameter_count = 1;
    version.layers_added = 1;
    version.has_other_changes = true;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Contrast · Structure"),
        "topology changes need a concise fallback alongside exact controls"
    );
}

void an_unrepresented_reported_change_is_not_lost() {
    BackendEditVersion version;
    version.changed_basic_parameter_count = 1;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Other adjustment"),
        "a reported change missing from the key list must remain visible"
    );
}

void names_and_parent_counts_do_not_reveal_ids() {
    BackendEditVersion version;
    version.commit_id = QStringLiteral("01900000-0000-7000-8000-000000000000");
    version.name = QStringLiteral("   ");
    version.parent_commit_ids = {QStringLiteral("parent-a"), QStringLiteral("parent-b")};
    require(
        EditVersionPresentation::displayName(version) == QStringLiteral("Untitled version"),
        "an empty name must use a semantic fallback instead of a commit id"
    );
    require(
        EditVersionPresentation::parentSummary(version) == QStringLiteral("2 parents"),
        "multi-parent lineage must preserve the exact parent count"
    );
}

void the_model_exposes_presentation_without_raw_keys() {
    BackendEditVersion version;
    version.name = QStringLiteral("Warm evening");
    version.parent_commit_ids = {QStringLiteral("parent")};
    version.changed_basic_parameters = {
        QStringLiteral("exposure_stops"),
        QStringLiteral("future_selective_color"),
    };
    version.changed_basic_parameter_count = 2;
    version.is_working = true;

    EditVersionModel model;
    model.replace({version});
    const auto index = model.index(0, 0);
    require(
        model.data(index, EditVersionModel::ChangeSummaryRole).toString()
            == QStringLiteral("Exposure · Other adjustment"),
        "the model must expose the composed summary role"
    );
    require(
        model.data(index, EditVersionModel::ParentSummaryRole).toString()
            == QStringLiteral("1 parent"),
        "the model must expose readable lineage"
    );
    require(
        model.data(index, EditVersionModel::ParentCountRole).toLongLong() == 1,
        "the existing exact parent count must remain available"
    );
    require(
        model.data(index, EditVersionModel::CurrentRole).toBool(),
        "the existing current-version state must remain available"
    );
    require(
        model.roleNames().value(EditVersionModel::ChangeSummaryRole)
            == QByteArrayLiteral("changeSummary"),
        "QML must receive the stable changeSummary role"
    );
}

} // namespace

int main() {
    root_has_a_semantic_summary();
    basic_keys_are_localizable_labels();
    every_basic_key_has_a_stable_label();
    unknown_keys_are_never_exposed_or_dropped();
    structural_and_basic_changes_compose();
    an_unrepresented_reported_change_is_not_lost();
    names_and_parent_counts_do_not_reveal_ids();
    the_model_exposes_presentation_without_raw_keys();
    return EXIT_SUCCESS;
}
