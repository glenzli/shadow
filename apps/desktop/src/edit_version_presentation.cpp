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
        return translated("Exposure");
    }
    if (key == QStringLiteral("contrast_factor")) {
        return translated("Contrast");
    }
    if (key == QStringLiteral("red_channel_gain")) {
        return translated("Red gain");
    }
    if (key == QStringLiteral("green_channel_gain")) {
        return translated("Green gain");
    }
    if (key == QStringLiteral("blue_channel_gain")) {
        return translated("Blue gain");
    }
    if (key == QStringLiteral("saturation_factor")) {
        return translated("Saturation");
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
    return version.recipe_schema_changed || version.layers_added > 0
        || version.layers_removed > 0 || version.layers_moved > 0
        || version.nodes_added > 0 || version.nodes_removed > 0;
}

} // namespace

namespace EditVersionPresentation {

QString displayName(const BackendEditVersion& version) {
    if (!version.name.trimmed().isEmpty()) {
        return version.name;
    }
    return translated("Untitled version");
}

QString changeSummary(const BackendEditVersion& version) {
    if (version.is_root) {
        return translated("Initial version");
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
        append_unique(labels, seen, translated("Structure"));
    }

    if (unknown_count > 0 || (version.has_other_changes && !topology_change)) {
        append_unique(
            labels,
            seen,
            unknown_count > 1
                ? translated("Other adjustments")
                : translated("Other adjustment")
        );
    }

    if (labels.isEmpty()) {
        return translated("Version checkpoint");
    }
    return labels.join(QStringLiteral(" · "));
}

QString parentSummary(const BackendEditVersion& version) {
    const auto parent_count = version.parent_commit_ids.size();
    if (parent_count == 0) {
        return translated("Root");
    }
    if (parent_count == 1) {
        return translated("1 parent");
    }
    return translated("%1 parents").arg(parent_count);
}

} // namespace EditVersionPresentation
