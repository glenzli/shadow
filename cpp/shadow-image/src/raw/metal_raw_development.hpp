#pragma once

#include <shadow/image/fused_raw_development.hpp>

#include <optional>
#include <string>

namespace shadow::image::detail {

struct MetalRawDevelopmentAttempt final {
    std::optional<FusedRawFrameDevelopment> development;
    std::string diagnostic;
};

// Availability is a runtime property: a macOS build may still run without a usable Metal device.
[[nodiscard]] bool metal_raw_development_available() noexcept;

// First Metal stage: native-size Bayer reconstruction, camera transform and orientation into the
// common linear-sRGB u16 boundary. Area-integrated previews intentionally remain on the exact CPU
// implementation until their floating footprint geometry has an integer formulation.
[[nodiscard]] MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_u16_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge
);

} // namespace shadow::image::detail
