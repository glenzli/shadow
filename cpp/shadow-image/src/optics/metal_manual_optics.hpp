#pragma once

#include <shadow/image/optics.hpp>

#include <optional>
#include <string>

namespace shadow::image::detail {

struct MetalManualSceneLinearOpticsAttempt final {
    std::optional<SceneLinearRgbFrame> corrected;
    std::string diagnostic;
};

// Executes the complete manual scene-linear distortion/TCA/vignetting contract. The caller owns
// product policy and CPU fallback; this owner contains only Metal admission and execution.
[[nodiscard]] MetalManualSceneLinearOpticsAttempt try_apply_manual_scene_linear_optics_metal(
    const SceneLinearRgbFrame& input,
    const OpticsSettings& settings
);

[[nodiscard]] bool metal_manual_scene_linear_optics_available() noexcept;

} // namespace shadow::image::detail
