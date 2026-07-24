#include "metal_display_output.hpp"

namespace shadow::image::detail {

bool metal_display_output_available() noexcept {
    return false;
}

MetalDisplayOutputAttempt try_render_linear_srgb_to_display_srgb8_metal(
    const FloatRgbImage&,
    const DisplayOutputRequest
) {
    return MetalDisplayOutputAttempt{
        .output = std::nullopt,
        .diagnostic = "Metal display output is not compiled for this platform",
    };
}

} // namespace shadow::image::detail
