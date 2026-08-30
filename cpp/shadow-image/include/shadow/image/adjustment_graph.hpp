#pragma once

#include <shadow/image/adjustment_parameters.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace shadow::image {

enum class AdjustmentOperation : std::uint8_t {
    exposure,
    contrast,
    oklab_lightness_tone_curve,
    oklab_opponent_tone_curves,
    rgb_white_balance,
    saturation,
    selective_tone,
    perceptual_color,
    oklab_color_warper,
    lut_3d,
    sharpen,
    spot_heal,
    image_completion,
};

inline constexpr std::uint32_t adjustment_parameter_schema_version = 1;
inline constexpr std::uint32_t adjustment_implementation_version = 1;

struct AdjustmentNode final {
    std::string node_id;
    std::uint32_t parameter_schema_version = adjustment_parameter_schema_version;
    std::uint32_t implementation_version = adjustment_implementation_version;
    bool enabled = true;
    AdjustmentParameters parameters = ExposureAdjustment{};
};

[[nodiscard]] AdjustmentOperation operation(const AdjustmentParameters& parameters) noexcept;
[[nodiscard]] std::string_view operation_id(AdjustmentOperation operation) noexcept;

} // namespace shadow::image
