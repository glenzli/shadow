#pragma once

#include <shadow/image/display_output.hpp>

#include <optional>
#include <string>

namespace shadow::image::detail {

struct MetalDisplayOutputAttempt final {
    std::optional<DisplayRgb8Image> output;
    std::string diagnostic;
};

// Availability is a runtime property: a macOS build can still run without a usable Metal device.
[[nodiscard]] bool metal_display_output_available() noexcept;

[[nodiscard]] MetalDisplayOutputAttempt try_render_linear_srgb_to_display_srgb8_metal(
    const FloatRgbImage& source,
    DisplayOutputRequest request
);

} // namespace shadow::image::detail
