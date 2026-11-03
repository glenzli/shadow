#include "adjustment_transfer_selection.hpp"

#include "edit_fine_parameter_registry.hpp"
#include "grade_node_display_label.hpp"

#include <QCoreApplication>
#include <QSet>
#include <stdexcept>

namespace {
QString label(const char* source) {
    return QCoreApplication::translate("AdjustmentTransferSelection", source);
}

template <typename Visitor>
void fields(const BackendGradeNode& source, BackendGradeNode& target, Visitor visit) {
    const BackendGradeNode neutral;
    const auto field = [&](const char* key,
                           const char* title,
                           const auto& value,
                           auto&& destination,
                           const auto& initial) {
        visit(QString::fromLatin1(key), label(title), value != initial, [&] {
            destination = value;
        });
    };
    field(
        "exposure",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Exposure"),
        source.basic.exposure_stops,
        target.basic.exposure_stops,
        neutral.basic.exposure_stops
    );
    field(
        "contrast",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Contrast"),
        source.basic.contrast_factor,
        target.basic.contrast_factor,
        neutral.basic.contrast_factor
    );
    field(
        "temperature",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Temperature"),
        source.basic.white_balance_temperature,
        target.basic.white_balance_temperature,
        neutral.basic.white_balance_temperature
    );
    field(
        "tint",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Tint"),
        source.basic.white_balance_tint,
        target.basic.white_balance_tint,
        neutral.basic.white_balance_tint
    );
    field(
        "saturation",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Saturation"),
        source.basic.saturation_factor,
        target.basic.saturation_factor,
        neutral.basic.saturation_factor
    );
    for (const auto& descriptor : EditFineParameterRegistry::all()) {
        // Coupled resources and defringe intervals travel with their complete
        // editable operation rather than as orphaned resource/interval values.
        if (!descriptor.label_source || descriptor.key == QLatin1StringView("lut_intensity")
            || descriptor.key == QLatin1StringView("color_warper_strength")
            || descriptor.key.startsWith(QLatin1StringView("defringe_")))
            continue;
        visit(
            QString(descriptor.key),
            QCoreApplication::translate("EditController", descriptor.label_source),
            source.fine.*descriptor.member != neutral.fine.*descriptor.member,
            [&] { target.fine.*descriptor.member = source.fine.*descriptor.member; }
        );
    }
    field(
        "mixer",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Color Mixer"),
        std::tie(source.fine.mixer_hue, source.fine.mixer_saturation, source.fine.mixer_lightness),
        std::tie(target.fine.mixer_hue, target.fine.mixer_saturation, target.fine.mixer_lightness),
        std::tie(
            neutral.fine.mixer_hue,
            neutral.fine.mixer_saturation,
            neutral.fine.mixer_lightness
        )
    );
    const auto pointColors = [](auto& fine) {
        return std::tie(
            fine.color_range_enabled,
            fine.color_range_center,
            fine.color_range_width,
            fine.color_range_softness,
            fine.color_range_hue,
            fine.color_range_saturation,
            fine.color_range_lightness,
            fine.additional_point_colors
        );
    };
    field(
        "point_colors",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Point Color"),
        pointColors(source.fine),
        pointColors(target.fine),
        pointColors(neutral.fine)
    );
    field(
        "selective_color",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Selective Color"),
        std::tie(source.fine.selective_color_relative, source.fine.selective_color_cmyk),
        std::tie(target.fine.selective_color_relative, target.fine.selective_color_cmyk),
        std::tie(neutral.fine.selective_color_relative, neutral.fine.selective_color_cmyk)
    );
    field(
        "perceptual_curve",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Perceptual curve"),
        source.fine.oklab_lightness_curve_points,
        target.fine.oklab_lightness_curve_points,
        neutral.fine.oklab_lightness_curve_points
    );
    field(
        "rgb_curves",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "RGB curves"),
        source.fine.rgb_curve_points,
        target.fine.rgb_curve_points,
        neutral.fine.rgb_curve_points
    );
    field(
        "color_warper",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Color Warper"),
        std::tie(
            source.fine.oklab_color_warper_control_points,
            source.fine.oklab_color_warper_strength
        ),
        std::tie(
            target.fine.oklab_color_warper_control_points,
            target.fine.oklab_color_warper_strength
        ),
        std::tie(
            neutral.fine.oklab_color_warper_control_points,
            neutral.fine.oklab_color_warper_strength
        )
    );
    const auto lut = [](auto& fine) {
        return std::tie(
            fine.lut_resource_id,
            fine.lut_title,
            fine.lut_managed_path,
            fine.lut_intensity
        );
    };
    field(
        "lut",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "LUT"),
        lut(source.fine),
        lut(target.fine),
        lut(neutral.fine)
    );
    const auto defringe = [](auto& fine) {
        return std::tie(
            fine.defringe_purple_amount,
            fine.defringe_purple_hue_low,
            fine.defringe_purple_hue_high,
            fine.defringe_green_amount,
            fine.defringe_green_hue_low,
            fine.defringe_green_hue_high
        );
    };
    field(
        "defringe",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Defringe"),
        defringe(source.fine),
        defringe(target.fine),
        defringe(neutral.fine)
    );
    field(
        "strength",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Node strength"),
        source.opacity,
        target.opacity,
        neutral.opacity
    );
    field(
        "enabled",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Node enabled state"),
        source.enabled,
        target.enabled,
        neutral.enabled
    );
    field(
        "mask",
        QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Mask"),
        std::tie(source.local_mask_components, source.local_mask_invert),
        std::tie(target.local_mask_components, target.local_mask_invert),
        std::tie(neutral.local_mask_components, neutral.local_mask_invert)
    );
}
} // namespace

QVariantList AdjustmentTransferSelection::options(const BackendGradeStack& stack) {
    QVariantList nodes;
    for (const auto& node : stack.grade_nodes) {
        QVariantList items;
        BackendGradeNode unused;
        fields(node, unused, [&](const QString& key, const QString& text, bool changed, auto) {
            if (changed)
                items.push_back(QVariantMap{{"key", key}, {"label", text}});
        });
        // Neutral nodes do not contribute a style and need no copy checkbox.
        if (!items.isEmpty())
            nodes.push_back(
                QVariantMap{
                    {"nodeId", node.grade_node_id},
                    {"label",
                     node.label.isEmpty()
                         ? label(
                               QT_TRANSLATE_NOOP("AdjustmentTransferSelection", "Adjustment node")
                           )
                         : GradeNodePresentation::display_grade_node_label(node.label)},
                    {"items", items}
                }
            );
    }
    return nodes;
}

AdjustmentTransferSource AdjustmentTransferSelection::select(
    const AdjustmentTransferSource& source,
    const QVariantList& choices
) {
    auto result = source;
    result.stack.grade_nodes.clear();
    QHash<QString, QSet<QString>> selected;
    for (const auto& value : choices) {
        const auto choice = value.toMap();
        const auto nodeId = choice.value("nodeId").toString();
        for (const auto& key : choice.value("keys").toList())
            selected[nodeId].insert(key.toString());
    }
    for (const auto& node : source.stack.grade_nodes) {
        auto keys = selected.take(node.grade_node_id);
        if (keys.isEmpty())
            continue;
        auto copy = node;
        copy.basic = {};
        copy.fine = {};
        copy.opacity = 1.0;
        copy.enabled = true;
        copy.local_mask_components.clear();
        copy.local_mask_invert = false;
        fields(node, copy, [&](const QString& key, const QString&, bool changed, auto apply) {
            if (keys.remove(key) && changed)
                apply();
        });
        if (!keys.isEmpty())
            throw std::invalid_argument(
                label(QT_TRANSLATE_NOOP(
                          "AdjustmentTransferSelection",
                          "The adjustment selection changed. Choose the settings again."
                      ))
                    .toStdString()
            );
        result.stack.grade_nodes.push_back(std::move(copy));
    }
    if (!selected.isEmpty() || result.stack.grade_nodes.isEmpty())
        throw std::invalid_argument(label(QT_TRANSLATE_NOOP(
                                              "AdjustmentTransferSelection",
                                              "Choose at least one modified adjustment setting."
                                          ))
                                        .toStdString());
    return result;
}
