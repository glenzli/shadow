#include "adjustment_execution_internal.hpp"

namespace shadow::image::detail {

bool metal_adjustment_available() noexcept {
    return false;
}

MetalAdjustmentAttempt try_execute_adjustments_metal(
    const FloatRgbImage&,
    const PreparedMetalAdjustment&
) {
    return MetalAdjustmentAttempt{
        .output = std::nullopt,
        .diagnostic = "Metal adjustment is not compiled for this platform",
    };
}

} // namespace shadow::image::detail
