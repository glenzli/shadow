#pragma once

#include <shadow/image/adjustment_execution.hpp>

#include <cstdint>
#include <span>

namespace shadow::image::adjustment_execution_contract::parity_fixture {

[[nodiscard]] shadow::image::FloatRgbImage make_image(
    std::uint32_t width,
    std::uint32_t height,
    bool padded = false
);
[[nodiscard]] bool close_to_cpu(
    const shadow::image::FloatRgbImage& actual,
    const shadow::image::FloatRgbImage& expected,
    double& maximum_error,
    double relative_tolerance = 3.0e-5
);
[[nodiscard]] std::uint8_t maximum_rgb8_difference(
    std::span<const std::uint8_t> actual,
    std::span<const std::uint8_t> expected
);

} // namespace shadow::image::adjustment_execution_contract::parity_fixture
