#include "metal_raw_development.hpp"

namespace shadow::image::detail {

bool metal_raw_development_available() noexcept {
    return false;
}

bool metal_raw_denoise_available() noexcept {
    return false;
}

bool metal_dcp_color_development_available() noexcept {
    return false;
}

MetalRawDenoiseAttempt
try_denoise_bayer_raw_frame_metal(RawFrame&, const RawBayerDenoiseMode, const double) {
    return MetalRawDenoiseAttempt{
        .applied = false,
        .diagnostic = "Metal RAW denoise is not compiled for this platform",
    };
}

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_f32_metal(
    const RawFrame&,
    const RawFrameLinearTransform&,
    const std::optional<std::uint32_t>,
    const RawHighlightRecoveryIntent,
    const RawDevelopmentQuality,
    const DcpColorTransform*
) {
    return MetalRawDevelopmentAttempt{
        .development = std::nullopt,
        .dcp_applied = false,
        .diagnostic = "Metal RAW development is not compiled for this platform",
    };
}

MetalDcpColorDevelopmentAttempt
try_apply_dcp_color_rendering_stages_metal(SceneLinearRgbFrame&, const DcpColorTransform&) {
    return MetalDcpColorDevelopmentAttempt{
        .applied = false,
        .diagnostic = "Metal DCP color development is not compiled for this platform",
    };
}

} // namespace shadow::image::detail
