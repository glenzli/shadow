#include "metal_manual_optics.hpp"

namespace shadow::image::detail {

MetalManualSceneLinearOpticsAttempt
try_apply_manual_scene_linear_optics_metal(const SceneLinearRgbFrame&, const OpticsSettings&) {
    return {
        .corrected = std::nullopt,
        .diagnostic = "Metal manual optics is not compiled for this platform",
    };
}

bool metal_manual_scene_linear_optics_available() noexcept {
    return false;
}

} // namespace shadow::image::detail
