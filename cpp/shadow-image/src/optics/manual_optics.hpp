#pragma once

#include <shadow/image/optics.hpp>

namespace shadow::image::detail::manual_optics {

void validate_settings(const OpticsSettings& settings);

void validate_manual_scene_linear_input(const SceneLinearRgbFrame& input);

[[nodiscard]] bool has_manual_geometry(const OpticsSettings& settings) noexcept;
[[nodiscard]] bool has_manual_vignetting(const OpticsSettings& settings) noexcept;

void apply_manual_scene_linear_vignetting_region(
    SceneLinearRgbFrame& input,
    Dimensions full_dimensions,
    std::uint32_t origin_x,
    std::uint32_t origin_y,
    const OpticsSettings& settings
);

[[nodiscard]] std::optional<PixelBuffer>
apply_manual_optics(const PixelBuffer& input, const OpticsSettings& settings);

[[nodiscard]] std::optional<SceneLinearRgbFrame>
apply_manual_optics(const SceneLinearRgbFrame& input, const OpticsSettings& settings);

[[nodiscard]] OpticsCorrectionResult with_manual_optics(
    OpticsProfileReceipt receipt,
    const PixelBuffer& input,
    const OpticsSettings& settings
);

[[nodiscard]] SceneLinearOpticsCorrectionResult with_manual_optics(
    OpticsProfileReceipt receipt,
    const SceneLinearRgbFrame& input,
    const OpticsSettings& settings
);

} // namespace shadow::image::detail::manual_optics
