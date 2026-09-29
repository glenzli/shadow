#include "edit_fine_parameter_registry.hpp"

#include <QCoreApplication>
#include <QSet>
#include <QString>

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void registry_is_complete_and_unique() {
    using namespace Qt::Literals::StringLiterals;
    constexpr auto expected_keys = std::to_array<QLatin1StringView>({
        "highlights"_L1,
        "shadows"_L1,
        "whites"_L1,
        "blacks"_L1,
        "highlight_red_suppression"_L1,
        "highlight_green_suppression"_L1,
        "highlight_blue_suppression"_L1,
        "global_a_balance"_L1,
        "global_b_balance"_L1,
        "vibrance"_L1,
        "color_warper_strength"_L1,
        "lut_intensity"_L1,
        "sharpen_amount"_L1,
        "sharpen_radius"_L1,
        "sharpen_threshold"_L1,
        "sharpen_masking"_L1,
        "clarity"_L1,
        "texture"_L1,
        "local_contrast"_L1,
        "local_contrast_scale"_L1,
        "selective_color_lightness_protection"_L1,
        "denoise_luminance"_L1,
        "denoise_detail"_L1,
        "denoise_color"_L1,
        "dehaze"_L1,
        "defringe_purple_amount"_L1,
        "defringe_purple_hue_low"_L1,
        "defringe_purple_hue_high"_L1,
        "defringe_green_amount"_L1,
        "defringe_green_hue_low"_L1,
        "defringe_green_hue_high"_L1,
        "shadows_hue"_L1,
        "shadows_saturation"_L1,
        "shadows_luminance"_L1,
        "midtones_hue"_L1,
        "midtones_saturation"_L1,
        "midtones_luminance"_L1,
        "highlights_hue"_L1,
        "highlights_saturation"_L1,
        "highlights_luminance"_L1,
        "grading_blending"_L1,
        "grading_balance"_L1,
        "grain_amount"_L1,
        "grain_size"_L1,
        "grain_roughness"_L1,
        "vignette_amount"_L1,
        "vignette_midpoint"_L1,
        "vignette_roundness"_L1,
        "vignette_feather"_L1,
        "vignette_highlights"_L1,
    });

    const auto descriptors = EditFineParameterRegistry::all();
    require(
        descriptors.size() == expected_keys.size(),
        "fine-parameter registry size changed without updating its contract"
    );

    QSet<QString> observed;
    for (const auto& descriptor : descriptors) {
        require(descriptor.member != nullptr, "parameter descriptor must own a field");
        require(
            std::isfinite(descriptor.minimum) && std::isfinite(descriptor.maximum)
                && descriptor.minimum <= descriptor.maximum,
            "parameter descriptor bounds must be finite and ordered"
        );
        const QString key(descriptor.key);
        require(!observed.contains(key), "parameter descriptor keys must be unique");
        observed.insert(key);
    }
    for (const auto key : expected_keys) {
        const QString key_string(key);
        require(
            EditFineParameterRegistry::find(QStringView{key_string}) != nullptr,
            "expected fine-parameter descriptor is missing"
        );
    }
    require(
        EditFineParameterRegistry::find(u"unknown_parameter") == nullptr,
        "unknown parameter keys must not resolve"
    );
}

void registry_projects_fields_and_write_policy() {
    using namespace Qt::Literals::StringLiterals;

    BackendFineEditParameters fine;
    const auto* const sharpen_radius = EditFineParameterRegistry::find(u"sharpen_radius");
    require(
        sharpen_radius != nullptr && sharpen_radius->writable(),
        "sharpen radius must remain writable"
    );
    require(
        sharpen_radius->minimum == 0.1 && sharpen_radius->maximum == 5.0,
        "sharpen radius bounds changed"
    );
    fine.*(sharpen_radius->member) = 2.75;
    require(fine.sharpen_radius == 2.75, "descriptor member must project the owned backend field");

    const auto* const highlight_red = EditFineParameterRegistry::find(u"highlight_red_suppression");
    require(
        highlight_red != nullptr && highlight_red->writable(),
        "highlight red suppression must remain writable"
    );
    require(
        highlight_red->minimum == 0.0 && highlight_red->maximum == 1.0,
        "highlight channel suppression bounds changed"
    );
    fine.*(highlight_red->member) = 0.75;
    require(
        fine.highlight_red_suppression == 0.75,
        "highlight suppression descriptor must project its backend field"
    );

    constexpr auto read_only_keys = std::to_array<QStringView>({
        u"defringe_purple_hue_low",
        u"defringe_purple_hue_high",
        u"defringe_green_hue_low",
        u"defringe_green_hue_high",
    });
    for (const auto key : read_only_keys) {
        const auto* const descriptor = EditFineParameterRegistry::find(key);
        require(
            descriptor != nullptr && !descriptor->writable(),
            "paired defringe hue endpoints must be read-only in the scalar registry"
        );
    }
    for (const auto& descriptor : EditFineParameterRegistry::all()) {
        if (descriptor.writable()) {
            require(
                descriptor.label_source != nullptr && descriptor.label_source[0] != '\0',
                "writable parameters require a localized validation label"
            );
        }
        require(
            !descriptor.key.startsWith("color_range_"_L1),
            "Point Color ranges must remain owned by the selected-range model"
        );
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        registry_is_complete_and_unique();
        registry_projects_fields_and_write_policy();
    } catch (const std::exception& error) {
        std::cerr << "edit fine parameter registry test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
