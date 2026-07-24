#include "metal_raw_development.hpp"

namespace shadow::image::detail {

bool metal_raw_development_available() noexcept {
    return false;
}

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_u16_metal(
    const RawFrame&,
    const RawFrameLinearTransform&,
    const std::optional<std::uint32_t>
) {
    return MetalRawDevelopmentAttempt{
        .development = std::nullopt,
        .diagnostic = "Metal RAW development is not compiled for this platform",
    };
}

} // namespace shadow::image::detail
