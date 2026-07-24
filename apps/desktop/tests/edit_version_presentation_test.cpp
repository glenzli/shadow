#include "edit_version_model.hpp"
#include "edit_version_presentation.hpp"
#include "localized_ui_message.hpp"

#include <QCoreApplication>
#include <QTranslator>

#include <cstdlib>
#include <cstring>
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
            == QStringLiteral("Exposure · Chroma"),
        "known parameter keys must become readable labels"
    );
}

void every_basic_key_has_a_stable_label() {
    BackendEditVersion version;
    version.changed_basic_parameters = {
        QStringLiteral("exposure_stops"),
        QStringLiteral("contrast_factor"),
        QStringLiteral("oklab_lightness_curve"),
        QStringLiteral("grade_node_enabled"),
        QStringLiteral("white_balance_temperature"),
        QStringLiteral("white_balance_tint"),
        QStringLiteral("saturation_factor"),
    };
    version.changed_basic_parameter_count = 7;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral(
                "Exposure · Contrast · Perceptual Lightness Curve · Grade Node bypass · "
                "Temperature · Tint · Chroma"
            ),
        "every renderer-backed edit parameter needs a stable display label"
    );
}

void perceptual_lightness_curve_has_a_stable_version_label() {
    BackendEditVersion version;
    version.changed_basic_parameters = {QStringLiteral("oklab_lightness_curve")};
    version.changed_basic_parameter_count = 1;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Perceptual Lightness Curve"),
        "perceptual lightness curve edits must not collapse into an opaque structural change"
    );
}

void sharpening_changes_share_one_stable_version_label() {
    BackendEditVersion grouped;
    grouped.changed_basic_parameters = {QStringLiteral("sharpening")};
    grouped.changed_basic_parameter_count = 1;
    require(
        EditVersionPresentation::changeSummary(grouped)
            == QStringLiteral("Sharpening"),
        "the grouped sharpening diff key needs a stable semantic label"
    );

    BackendEditVersion defensive_individual_keys;
    defensive_individual_keys.changed_basic_parameters = {
        QStringLiteral("sharpen_amount"),
        QStringLiteral("sharpen_radius"),
        QStringLiteral("sharpen_threshold"),
        QStringLiteral("sharpen_masking"),
    };
    defensive_individual_keys.changed_basic_parameter_count = 4;
    require(
        EditVersionPresentation::changeSummary(defensive_individual_keys)
            == QStringLiteral("Sharpening"),
        "sharpening controls must collapse into one readable version label"
    );
}

void grade_node_bypass_has_a_stable_version_label() {
    BackendEditVersion version;
    version.changed_basic_parameters = {QStringLiteral("grade_node_enabled")};
    version.changed_basic_parameter_count = 1;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Grade Node bypass"),
        "Grade Node state changes need a stable semantic label"
    );
}

void optics_has_a_stable_version_label() {
    BackendEditVersion version;
    version.changed_basic_parameters = {QStringLiteral("optics")};
    version.changed_basic_parameter_count = 1;
    require(
        EditVersionPresentation::changeSummary(version) == QStringLiteral("Optics"),
        "input-stage optical changes need a stable semantic label"
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
    version.grade_nodes_added = 1;
    version.has_other_changes = true;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Contrast · Structure"),
        "topology changes need a concise fallback alongside exact controls"
    );
}

void internal_render_ops_are_not_presented_as_user_structure() {
    BackendEditVersion version;
    version.render_ops_added = 2;
    version.render_ops_removed = 1;
    version.render_ops_modified = 3;
    version.render_op_parameter_blocks_changed = 2;
    version.has_other_changes = true;
    const QString summary = EditVersionPresentation::changeSummary(version);
    require(
        summary == QStringLiteral("Other adjustment"),
        "internal Render Op churn must remain outside user-visible Grade Node topology"
    );
    require(
        !summary.contains(QStringLiteral("Structure")),
        "only Grade Node add, remove, or move may be presented as Structure"
    );
}

void grade_node_payload_changes_are_not_topology_changes() {
    BackendEditVersion version;
    version.grade_nodes_modified = 1;
    version.has_other_changes = true;
    require(
        EditVersionPresentation::changeSummary(version)
            == QStringLiteral("Other adjustment"),
        "modifying a Grade Node payload must not masquerade as topology"
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
    version.is_selected = true;

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
        model.data(index, EditVersionModel::SelectedRole).toBool(),
        "the selected version state must remain available"
    );
    require(
        model.roleNames().value(EditVersionModel::ChangeSummaryRole)
            == QByteArrayLiteral("changeSummary"),
        "QML must receive the stable changeSummary role"
    );
}

void selected_version_marker_moves_between_loaded_draft_and_durable_head() {
    BackendEditVersion durable;
    durable.commit_id = QStringLiteral("durable-head");
    durable.name = QStringLiteral("Latest version");
    durable.is_selected = true;
    BackendEditVersion loaded;
    loaded.commit_id = QStringLiteral("loaded-base");
    loaded.name = QStringLiteral("Earlier version");

    EditVersionModel model;
    model.replace({durable, loaded});
    int change_count = 0;
    QList<int> changed_roles;
    QObject::connect(
        &model,
        &QAbstractItemModel::dataChanged,
        [&change_count, &changed_roles](
            const QModelIndex& first,
            const QModelIndex& last,
            const QList<int>& roles
        ) {
            require(
                first.row() == 0 && last.row() == 1,
                "selection changes must invalidate the complete version range"
            );
            ++change_count;
            changed_roles = roles;
        }
    );

    model.setSelectedCommit(QStringLiteral("loaded-base"));
    require(
        !model.data(model.index(0, 0), EditVersionModel::SelectedRole).toBool()
            && model.data(model.index(1, 0), EditVersionModel::SelectedRole).toBool(),
        "loading an old version must move only the selected presentation marker"
    );
    require(
        change_count == 1 && changed_roles == QList<int>{EditVersionModel::SelectedRole},
        "loading a version draft must notify only the semantic selection role"
    );

    model.setSelectedCommit(QStringLiteral("durable-head"));
    require(
        model.data(model.index(0, 0), EditVersionModel::SelectedRole).toBool()
            && !model.data(model.index(1, 0), EditVersionModel::SelectedRole).toBool(),
        "discarding a draft must restore the durable head marker"
    );
    require(change_count == 2, "restoring the durable marker must notify the view");
}

class TestTranslator final : public QTranslator {
public:
  [[nodiscard]] bool isEmpty() const override { return false; }

  [[nodiscard]] QString translate(const char *const context,
                                  const char *const source_text,
                                  const char *const, const int) const override {
    if (std::strcmp(context, "EditVersionModel") == 0) {
      if (std::strcmp(source_text, "Exposure") == 0) {
        return QStringLiteral("曝光");
      }
      if (std::strcmp(source_text, "Other adjustment") == 0) {
        return QStringLiteral("其他调整");
      }
      if (std::strcmp(source_text, "Untitled version") == 0) {
        return QStringLiteral("未命名版本");
      }
      if (std::strcmp(source_text, "1 parent") == 0) {
        return QStringLiteral("1 个父版本");
      }
    }
    if (std::strcmp(context, "MessageTest") == 0 &&
        std::strcmp(source_text, "Operation failed · %1 · %2") == 0) {
      return QStringLiteral("操作失败 · %1 · %2");
    }
    if (std::strcmp(context, "MessageTest") == 0 &&
        std::strcmp(source_text, "Reordered · %1 · %2") == 0) {
      return QStringLiteral("重排 · %2 · %1 · %2");
    }
    return {};
  }
};

void semantic_messages_retranslate_without_touching_raw_arguments() {
  const QString raw_detail =
      QStringLiteral("decoder failed at /photos/%2/DSC_%1.NEF");
  const LocalizedUiMessage message{
      "MessageTest",
      QT_TRANSLATE_NOOP("MessageTest", "Operation failed · %1 · %2"),
      {raw_detail, 17},
  };
  require(
      message.translated() ==
          QStringLiteral(
              "Operation failed · decoder failed at /photos/%2/DSC_%1.NEF · 17"),
      "the source-language message must retain typed raw arguments without "
      "rescanning placeholder-shaped diagnostics");

  TestTranslator translator;
  require(QCoreApplication::installTranslator(&translator),
          "the test translator must install");
  require(message.translated() ==
              QStringLiteral(
                  "操作失败 · decoder failed at /photos/%2/DSC_%1.NEF · 17"),
          "only the stable wrapper may translate; raw diagnostics must remain "
          "verbatim");
  require(QCoreApplication::removeTranslator(&translator),
          "the test translator must uninstall");
}

void semantic_message_placeholders_follow_only_the_template() {
  const LocalizedUiMessage reordered{
      "MessageTest",
      QT_TRANSLATE_NOOP("MessageTest", "Reordered · %1 · %2"),
      {QStringLiteral("raw %1 %2"), QStringLiteral("second")},
  };
  TestTranslator translator;
  require(QCoreApplication::installTranslator(&translator),
          "the reordered-message translator must install");
  require(reordered.translated() ==
              QStringLiteral("重排 · second · raw %1 %2 · second"),
          "translated templates may reorder and repeat placeholders without "
          "rescanning inserted arguments");
  require(QCoreApplication::removeTranslator(&translator),
          "the reordered-message translator must uninstall");

  const LocalizedUiMessage missing{
      "MessageTest",
      QT_TRANSLATE_NOOP("MessageTest", "Present %1 · missing %3"),
      {QStringLiteral("value"), QStringLiteral("unused")},
  };
  require(missing.translated() == QStringLiteral("Present value · missing %3"),
          "a placeholder without an argument must remain visible");

  const LocalizedUiMessage extra{
      "MessageTest",
      QT_TRANSLATE_NOOP("MessageTest", "Only %1"),
      {QStringLiteral("value"), QStringLiteral("unused")},
  };
  require(extra.translated() == QStringLiteral("Only value"),
          "extra typed arguments must not alter the template");
}

void version_model_retranslates_presentation_roles_only() {
  BackendEditVersion version;
  version.commit_id = QStringLiteral("opaque-commit-id");
  version.name = QStringLiteral("Warm evening");
  version.parent_commit_ids = {QStringLiteral("opaque-parent-id")};
  version.changed_basic_parameters = {
      QStringLiteral("exposure_stops"),
      QStringLiteral("future_selective_color"),
  };
  version.changed_basic_parameter_count = 2;

  EditVersionModel model;
  model.replace({version});
  bool changed = false;
  QList<int> changed_roles;
  QObject::connect(&model, &QAbstractItemModel::dataChanged,
                   [&changed, &changed_roles](const QModelIndex &first,
                                              const QModelIndex &last,
                                              const QList<int> &roles) {
                     changed = first.row() == 0 && last.row() == 0;
                     changed_roles = roles;
                   });

  TestTranslator translator;
  require(QCoreApplication::installTranslator(&translator),
          "the version translator must install");
  const auto index = model.index(0, 0);
  require(changed,
          "LanguageChange must automatically invalidate visible version rows");
  require(changed_roles.contains(EditVersionModel::LabelRole) &&
              changed_roles.contains(EditVersionModel::ChangeSummaryRole) &&
              changed_roles.contains(EditVersionModel::ParentSummaryRole),
          "language refresh must identify every translated presentation role");
  require(model.data(index, EditVersionModel::LabelRole).toString() ==
              QStringLiteral("Warm evening"),
          "user-supplied version names must never be translated");
  require(model.data(index, EditVersionModel::ChangeSummaryRole).toString() ==
              QStringLiteral("曝光 · 其他调整"),
          "semantic change labels must be recomputed in the active language");
  require(model.data(index, EditVersionModel::ParentSummaryRole).toString() ==
              QStringLiteral("1 个父版本"),
          "lineage presentation must be recomputed in the active language");
  require(model.data(index, EditVersionModel::CommitIdRole).toString() ==
              QStringLiteral("opaque-commit-id"),
          "opaque identifiers must remain untouched");
  changed = false;
  changed_roles.clear();
  require(QCoreApplication::removeTranslator(&translator),
          "the version translator must uninstall");
  require(changed &&
              changed_roles.contains(EditVersionModel::ChangeSummaryRole),
          "removing a translator must automatically restore source-language "
          "version roles");
  require(model.data(index, EditVersionModel::ChangeSummaryRole).toString() ==
              QStringLiteral("Exposure · Other adjustment"),
          "source-language version presentation must return after removal");
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  root_has_a_semantic_summary();
    basic_keys_are_localizable_labels();
    every_basic_key_has_a_stable_label();
    perceptual_lightness_curve_has_a_stable_version_label();
    sharpening_changes_share_one_stable_version_label();
    grade_node_bypass_has_a_stable_version_label();
    optics_has_a_stable_version_label();
    unknown_keys_are_never_exposed_or_dropped();
    structural_and_basic_changes_compose();
    internal_render_ops_are_not_presented_as_user_structure();
    grade_node_payload_changes_are_not_topology_changes();
    an_unrepresented_reported_change_is_not_lost();
    names_and_parent_counts_do_not_reveal_ids();
  the_model_exposes_presentation_without_raw_keys();
  selected_version_marker_moves_between_loaded_draft_and_durable_head();
  semantic_messages_retranslate_without_touching_raw_arguments();
  semantic_message_placeholders_follow_only_the_template();
  version_model_retranslates_presentation_roles_only();
  return EXIT_SUCCESS;
}
