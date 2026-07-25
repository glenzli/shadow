#include "metal_raw_development.hpp"

namespace shadow::image::detail {

bool metal_raw_development_available() noexcept {
    return false;
}

bool metal_raw_denoise_available() noexcept {
    return false;
}

MetalRawDenoiseAttempt try_denoise_bayer_raw_frame_metal(
    RawFrame&,
    const RawBayerDenoiseMode,
    const double
) {
    return MetalRawDenoiseAttempt{
        .applied = false,
        .diagnostic = "Metal RAW denoise is not compiled for this platform",
    };
}

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_u16_metal(
    const RawFrame&,
    const RawFrameLinearTransform&,
    const std::optional<std::uint32_t>,
    const RawHighlightRecoveryIntent
) {
    return MetalRawDevelopmentAttempt{
        .development = std::nullopt,
        .diagnostic = "Metal RAW development is not compiled for this platform",
    };
}

} // namespace shadow::image::detail
