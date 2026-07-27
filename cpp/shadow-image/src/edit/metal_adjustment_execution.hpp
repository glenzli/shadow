#pragma once

#include "metal_adjustment_program.hpp"

#include <optional>
#include <string>

namespace shadow::image::detail {

struct MetalAdjustmentAttempt final {
    std::optional<FloatRgbImage> output;
    std::string diagnostic;
};

[[nodiscard]] bool metal_adjustment_available() noexcept;

[[nodiscard]] MetalAdjustmentAttempt try_execute_adjustments_metal(
    const FloatRgbImage& input,
    const PreparedMetalAdjustment& program
);

} // namespace shadow::image::detail
