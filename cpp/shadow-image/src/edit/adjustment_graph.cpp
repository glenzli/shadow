#include <shadow/image/adjustment_graph.hpp>

#include <type_traits>
#include <variant>

namespace shadow::image {

AdjustmentOperation operation(const AdjustmentParameters& parameters) noexcept {
    return std::visit(
        [](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                return AdjustmentOperation::exposure;
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                return AdjustmentOperation::contrast;
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                return AdjustmentOperation::oklab_lightness_tone_curve;
            } else if constexpr (std::is_same_v<Parameters, OklabOpponentToneCurves>) {
                return AdjustmentOperation::oklab_opponent_tone_curves;
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                return AdjustmentOperation::rgb_white_balance;
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                return AdjustmentOperation::saturation;
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return AdjustmentOperation::selective_tone;
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                return AdjustmentOperation::perceptual_color;
            } else if constexpr (std::is_same_v<Parameters, OklabColorWarperAdjustment>) {
                return AdjustmentOperation::oklab_color_warper;
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                return AdjustmentOperation::lut_3d;
            } else if constexpr (std::is_same_v<Parameters, SpotHealAdjustment>) {
                return AdjustmentOperation::spot_heal;
            } else if constexpr (std::is_same_v<Parameters, ImageCompletionAdjustment>) {
                return AdjustmentOperation::image_completion;
            } else {
                static_assert(std::is_same_v<Parameters, SharpenAdjustment>);
                return AdjustmentOperation::sharpen;
            }
        },
        parameters
    );
}

std::string_view operation_id(const AdjustmentOperation operation) noexcept {
    switch (operation) {
    case AdjustmentOperation::exposure:
        return "shadow.exposure";
    case AdjustmentOperation::contrast:
        return "shadow.contrast";
    case AdjustmentOperation::oklab_lightness_tone_curve:
        return "shadow.oklab_lightness_tone_curve";
    case AdjustmentOperation::oklab_opponent_tone_curves:
        return "shadow.oklab_opponent_tone_curves";
    case AdjustmentOperation::rgb_white_balance:
        return "shadow.rgb_white_balance";
    case AdjustmentOperation::saturation:
        return "shadow.saturation";
    case AdjustmentOperation::selective_tone:
        return "shadow.selective_tone";
    case AdjustmentOperation::perceptual_color:
        return "shadow.perceptual_color";
    case AdjustmentOperation::oklab_color_warper:
        return "shadow.oklab_color_warper";
    case AdjustmentOperation::lut_3d:
        return "shadow.lut_3d";
    case AdjustmentOperation::sharpen:
        return "shadow.sharpen";
    case AdjustmentOperation::spot_heal:
        return "shadow.spot_heal";
    case AdjustmentOperation::image_completion:
        return "shadow.image_completion";
    }
    return "shadow.unknown";
}

} // namespace shadow::image
