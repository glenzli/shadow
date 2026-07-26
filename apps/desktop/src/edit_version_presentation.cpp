#include "edit_version_presentation.hpp"

#include <QCoreApplication>
#include <QSet>
#include <QStringList>

#include <cstdint>

namespace {

[[nodiscard]] QString translated(const char* const source) {
    return QCoreApplication::translate("EditVersionModel", source);
}

[[nodiscard]] QString basic_parameter_label(const QString& key) {
    if (key == QStringLiteral("exposure_stops")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Exposure"));
    }
    if (key == QStringLiteral("contrast_factor")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Contrast"));
    }
    if (key == QStringLiteral("oklab_lightness_curve")) {
        return translated(
            QT_TRANSLATE_NOOP("EditVersionModel", "Perceptual Lightness Curve")
        );
    }
    if (key == QStringLiteral("color_warper")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Color Map"));
    }
    if (key == QStringLiteral("grade_node_enabled")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Grade Node bypass"));
    }
    if (key == QStringLiteral("white_balance_temperature")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Temperature"));
    }
    if (key == QStringLiteral("white_balance_tint")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Tint"));
    }
    if (key == QStringLiteral("saturation_factor")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Chroma"));
    }
    if (key == QStringLiteral("highlights")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Highlights"));
    }
    if (key == QStringLiteral("shadows")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Shadows"));
    }
    if (key == QStringLiteral("whites")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Whites"));
    }
    if (key == QStringLiteral("blacks")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Blacks"));
    }
    if (key == QStringLiteral("vibrance")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Vibrance"));
    }
    if (key == QStringLiteral("color_mixer_hue")
        || key == QStringLiteral("color_mixer_saturation")
        || key == QStringLiteral("color_mixer_lightness")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Color Mixer"));
    }
    if (key == QStringLiteral("color_range")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Point Color"));
    }
    if (key == QStringLiteral("selective_color")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Selective Color"));
    }
    if (key == QStringLiteral("lut")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "LUT"));
    }
    if (key == QStringLiteral("optics")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Optics"));
    }
    if (key == QStringLiteral("sharpening")
        || key == QStringLiteral("sharpen_amount")
        || key == QStringLiteral("sharpen_radius")
        || key == QStringLiteral("sharpen_threshold")
        || key == QStringLiteral("sharpen_masking")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Sharpening"));
    }
    if (key == QStringLiteral("clarity") || key == QStringLiteral("texture")) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Perceptual Detail"));
    }
    return {};
}

void append_unique(QStringList& labels, QSet<QString>& seen, QString label) {
    if (!seen.contains(label)) {
        seen.insert(label);
        labels.push_back(std::move(label));
    }
}

[[nodiscard]] bool has_topology_change(const BackendEditVersion& version) {
    return version.recipe_schema_changed || version.grade_nodes_added > 0
        || version.grade_nodes_removed > 0 || version.grade_nodes_moved > 0;
}

} // namespace

namespace EditVersionPresentation {

QString displayName(const BackendEditVersion& version) {
    if (!version.name.trimmed().isEmpty()) {
        return version.name;
    }
    return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Untitled version"));
}

QString changeSummary(const BackendEditVersion& version) {
    if (version.is_root) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Initial version"));
    }

    QStringList labels;
    QSet<QString> seen;
    std::uint64_t unknown_count = 0;
    for (const auto& key : version.changed_basic_parameters) {
        auto label = basic_parameter_label(key);
        if (label.isEmpty()) {
            ++unknown_count;
        } else {
            append_unique(labels, seen, std::move(label));
        }
    }

    const auto observed_count = static_cast<std::uint64_t>(
        version.changed_basic_parameters.size()
    );
    const auto reported_count = static_cast<std::uint64_t>(
        version.changed_basic_parameter_count
    );
    if (reported_count > observed_count) {
        unknown_count += reported_count - observed_count;
    }

    const bool topology_change = has_topology_change(version);
    if (topology_change) {
        append_unique(labels, seen, translated(QT_TRANSLATE_NOOP("EditVersionModel", "Structure")));
    }

    if (unknown_count > 0 || (version.has_other_changes && !topology_change)) {
        append_unique(
            labels,
            seen,
            unknown_count > 1
                ? translated(QT_TRANSLATE_NOOP("EditVersionModel",
                                                     "Other adjustments"))
                : translated(QT_TRANSLATE_NOOP("EditVersionModel",
                                                     "Other adjustment"))
        );
    }

    if (labels.isEmpty()) {
        return translated(
        QT_TRANSLATE_NOOP("EditVersionModel", "Version checkpoint"));
    }
    return labels.join(QStringLiteral(" · "));
}

QString parentSummary(const BackendEditVersion& version) {
    const auto parent_count = version.parent_commit_ids.size();
    if (parent_count == 0) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "Root"));
    }
    if (parent_count == 1) {
        return translated(QT_TRANSLATE_NOOP("EditVersionModel", "1 parent"));
    }
    return translated(QT_TRANSLATE_NOOP("EditVersionModel", "%1 parents")).arg(parent_count);
}

} // namespace EditVersionPresentation
