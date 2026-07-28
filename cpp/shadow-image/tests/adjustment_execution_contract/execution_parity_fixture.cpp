#include "execution_parity_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract::parity_fixture {

[[nodiscard]] image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] image::FloatRgbImage make_image(
    const std::uint32_t width,
    const std::uint32_t height,
    const bool padded
) {
    const std::size_t active_row = static_cast<std::size_t>(width) * 3U;
    const std::size_t row_floats = active_row + (padded ? 5U : 0U);
    image::FloatRgbImage result{
        .dimensions = {width, height},
        .row_stride_bytes = row_floats * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_srgb(),
        .samples = std::vector<float>(row_floats * height, 17.0F),
    };
    std::mt19937 generator(0x5a17U);
    std::uniform_real_distribution<float> samples(-0.15F, 2.25F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * row_floats;
        for (std::size_t index = 0U; index < active_row; ++index) {
            result.samples[row + index] = samples(generator);
        }
    }
    if (width > 1U && height > 0U) {
        result.samples[0] = -0.05F;
        result.samples[1] = 0.20F;
        result.samples[2] = 1.75F;
        result.samples[3] = 2.0F;
        result.samples[4] = 2.0F;
        result.samples[5] = 2.0F;
    }
    return result;
}

[[nodiscard]] bool close_to_cpu(
    const image::FloatRgbImage& actual,
    const image::FloatRgbImage& expected,
    double& maximum_error,
    const double relative_tolerance
) {
    if (actual.dimensions != expected.dimensions
        || actual.row_stride_bytes != expected.row_stride_bytes
        || actual.samples.size() != expected.samples.size()) {
        return false;
    }
    maximum_error = 0.0;
    for (std::size_t index = 0U; index < expected.samples.size(); ++index) {
        const double reference = expected.samples[index];
        const double difference = std::abs(
            static_cast<double>(actual.samples[index]) - reference
        );
        maximum_error = std::max(maximum_error, difference);
        const double tolerance =
            relative_tolerance * std::max(1.0, std::abs(reference));
        if (difference > tolerance) {
            std::cerr << "Parity mismatch at " << index << ": actual="
                      << actual.samples[index] << ", CPU=" << expected.samples[index]
                      << ", tolerance=" << tolerance << '\n';
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint8_t maximum_rgb8_difference(
    const std::span<const std::uint8_t> actual,
    const std::span<const std::uint8_t> expected
) {
    if (actual.size() != expected.size()) {
        return std::numeric_limits<std::uint8_t>::max();
    }
    std::uint8_t maximum = 0U;
    for (std::size_t index = 0U; index < actual.size(); ++index) {
        maximum = std::max(
            maximum,
            static_cast<std::uint8_t>(std::abs(
                static_cast<int>(actual[index])
                - static_cast<int>(expected[index])
            ))
        );
    }
    return maximum;
}

} // namespace shadow::image::adjustment_execution_contract::parity_fixture
