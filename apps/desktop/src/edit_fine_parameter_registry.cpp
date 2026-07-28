#include "edit_fine_parameter_registry.hpp"

#include <QCoreApplication>

#include <array>
#include <ranges>

namespace {

using namespace Qt::Literals::StringLiterals;

constexpr auto PARAMETERS = std::to_array<EditFineParameterDescriptor>({
    {"highlights"_L1, &BackendFineEditParameters::highlights, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Highlights")},
    {"shadows"_L1, &BackendFineEditParameters::shadows, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Shadows")},
    {"whites"_L1, &BackendFineEditParameters::whites, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Whites")},
    {"blacks"_L1, &BackendFineEditParameters::blacks, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Blacks")},
    {"global_a_balance"_L1, &BackendFineEditParameters::global_a_balance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Green to red balance")},
    {"global_b_balance"_L1, &BackendFineEditParameters::global_b_balance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Blue to yellow balance")},
    {"vibrance"_L1, &BackendFineEditParameters::vibrance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Vibrance")},
    {"color_warper_strength"_L1,
     &BackendFineEditParameters::oklab_color_warper_strength, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Color Warper strength")},
    {"lut_intensity"_L1, &BackendFineEditParameters::lut_intensity, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "LUT intensity")},
    {"sharpen_amount"_L1, &BackendFineEditParameters::sharpen_amount, 0.0, 2.0,
     QT_TRANSLATE_NOOP("EditController", "Sharpening amount")},
    {"sharpen_radius"_L1, &BackendFineEditParameters::sharpen_radius, 0.1, 5.0,
     QT_TRANSLATE_NOOP("EditController", "Sharpening radius")},
    {"sharpen_threshold"_L1, &BackendFineEditParameters::sharpen_threshold, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Sharpening threshold")},
    {"sharpen_masking"_L1, &BackendFineEditParameters::sharpen_masking, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Sharpening masking")},
    {"clarity"_L1, &BackendFineEditParameters::clarity, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Perceptual clarity")},
    {"texture"_L1, &BackendFineEditParameters::texture, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Perceptual texture")},
    {"local_contrast"_L1, &BackendFineEditParameters::local_contrast, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Local contrast")},
    {"local_contrast_scale"_L1, &BackendFineEditParameters::local_contrast_scale, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Local contrast scale")},
    {"selective_color_lightness_protection"_L1,
     &BackendFineEditParameters::selective_color_lightness_protection, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Selective Color lightness protection")},
    {"denoise_luminance"_L1, &BackendFineEditParameters::denoise_luminance, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Luminance noise reduction")},
    {"denoise_detail"_L1, &BackendFineEditParameters::denoise_detail, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Noise reduction detail")},
    {"denoise_color"_L1, &BackendFineEditParameters::denoise_color, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Color noise reduction")},
    {"dehaze"_L1, &BackendFineEditParameters::dehaze, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Dehaze")},
    {"defringe_purple_amount"_L1, &BackendFineEditParameters::defringe_purple_amount,
     0.0, 1.0, QT_TRANSLATE_NOOP("EditController", "Purple defringe amount")},
    {"defringe_purple_hue_low"_L1, &BackendFineEditParameters::defringe_purple_hue_low,
     0.0, 360.0, nullptr},
    {"defringe_purple_hue_high"_L1, &BackendFineEditParameters::defringe_purple_hue_high,
     0.0, 360.0, nullptr},
    {"defringe_green_amount"_L1, &BackendFineEditParameters::defringe_green_amount,
     0.0, 1.0, QT_TRANSLATE_NOOP("EditController", "Green defringe amount")},
    {"defringe_green_hue_low"_L1, &BackendFineEditParameters::defringe_green_hue_low,
     0.0, 360.0, nullptr},
    {"defringe_green_hue_high"_L1, &BackendFineEditParameters::defringe_green_hue_high,
     0.0, 360.0, nullptr},
    {"shadows_hue"_L1, &BackendFineEditParameters::shadows_hue, 0.0, 360.0,
     QT_TRANSLATE_NOOP("EditController", "Shadow grading hue")},
    {"shadows_saturation"_L1, &BackendFineEditParameters::shadows_saturation, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Shadow grading saturation")},
    {"shadows_luminance"_L1, &BackendFineEditParameters::shadows_luminance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Shadow grading luminance")},
    {"midtones_hue"_L1, &BackendFineEditParameters::midtones_hue, 0.0, 360.0,
     QT_TRANSLATE_NOOP("EditController", "Midtone grading hue")},
    {"midtones_saturation"_L1, &BackendFineEditParameters::midtones_saturation, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Midtone grading saturation")},
    {"midtones_luminance"_L1, &BackendFineEditParameters::midtones_luminance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Midtone grading luminance")},
    {"highlights_hue"_L1, &BackendFineEditParameters::highlights_hue, 0.0, 360.0,
     QT_TRANSLATE_NOOP("EditController", "Highlight grading hue")},
    {"highlights_saturation"_L1, &BackendFineEditParameters::highlights_saturation, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Highlight grading saturation")},
    {"highlights_luminance"_L1, &BackendFineEditParameters::highlights_luminance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Highlight grading luminance")},
    {"grading_blending"_L1, &BackendFineEditParameters::grading_blending, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Color grading blending")},
    {"grading_balance"_L1, &BackendFineEditParameters::grading_balance, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Color grading balance")},
    {"grain_amount"_L1, &BackendFineEditParameters::grain_amount, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Grain amount")},
    {"grain_size"_L1, &BackendFineEditParameters::grain_size, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Grain size")},
    {"grain_roughness"_L1, &BackendFineEditParameters::grain_roughness, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Grain roughness")},
    {"vignette_amount"_L1, &BackendFineEditParameters::vignette_amount, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Vignette amount")},
    {"vignette_midpoint"_L1, &BackendFineEditParameters::vignette_midpoint, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Vignette midpoint")},
    {"vignette_roundness"_L1, &BackendFineEditParameters::vignette_roundness, -1.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Vignette roundness")},
    {"vignette_feather"_L1, &BackendFineEditParameters::vignette_feather, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Vignette feather")},
    {"vignette_highlights"_L1, &BackendFineEditParameters::vignette_highlights, 0.0, 1.0,
     QT_TRANSLATE_NOOP("EditController", "Vignette highlights")},
});

} // namespace

std::span<const EditFineParameterDescriptor> EditFineParameterRegistry::all() noexcept {
    return PARAMETERS;
}

const EditFineParameterDescriptor* EditFineParameterRegistry::find(
    const QStringView key
) noexcept {
    const auto descriptor = std::ranges::find_if(
        PARAMETERS,
        [key](const EditFineParameterDescriptor& candidate) {
            return key == candidate.key;
        }
    );
    return descriptor == PARAMETERS.end() ? nullptr : &*descriptor;
}
