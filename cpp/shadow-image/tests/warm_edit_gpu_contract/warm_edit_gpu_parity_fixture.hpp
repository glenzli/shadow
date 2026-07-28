#pragma once

#include <shadow/image/adjustment_execution.hpp>

#include <cstdint>
#include <span>

namespace shadow::image::warm_edit_gpu_contract::parity_fixture {

struct Rgb8Difference final {
    std::uint8_t maximum = 0U;
    std::uint8_t p99 = 0U;
};

[[nodiscard]] WorkingRgbSpace linear_srgb();
[[nodiscard]] FloatRgbImage make_random_image(
    std::uint32_t width,
    std::uint32_t height,
    bool padded
);
[[nodiscard]] bool linear_close(
    const FloatRgbImage& actual,
    const FloatRgbImage& expected,
    double& maximum_error,
    double relative_tolerance = 6.0e-5
);
[[nodiscard]] Rgb8Difference rgb8_difference(
    std::span<const std::uint8_t> actual,
    std::span<const std::uint8_t> expected
);

} // namespace shadow::image::warm_edit_gpu_contract::parity_fixture
