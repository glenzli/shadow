#include "warm_edit_gpu_parity_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <ranges>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::warm_edit_gpu_contract::parity_fixture {

[[nodiscard]] image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] image::FloatRgbImage make_random_image(
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
    std::mt19937 generator(0x914dU);
    std::uniform_real_distribution<float> samples(-0.15F, 2.25F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * row_floats;
        for (std::size_t index = 0U; index < active_row; ++index) {
            result.samples[row + index] = samples(generator);
        }
    }
    return result;
}

[[nodiscard]] bool linear_close(
    const image::FloatRgbImage& actual,
    const image::FloatRgbImage& expected,
    double& maximum_error,
    const double relative_tolerance
) {
    if (actual.dimensions != expected.dimensions
        || actual.row_stride_bytes % sizeof(float) != 0U
        || expected.row_stride_bytes % sizeof(float) != 0U) {
        return false;
    }
    maximum_error = 0.0;
    bool close = true;
    const std::size_t actual_row_floats =
        actual.row_stride_bytes / sizeof(float);
    const std::size_t expected_row_floats =
        expected.row_stride_bytes / sizeof(float);
    const std::size_t active_row =
        static_cast<std::size_t>(actual.dimensions.width) * 3U;
    if (actual_row_floats < active_row || expected_row_floats < active_row
        || actual.samples.size()
            != actual_row_floats * actual.dimensions.height
        || expected.samples.size()
            != expected_row_floats * expected.dimensions.height) {
        return false;
    }
    for (std::uint32_t y = 0U; y < actual.dimensions.height; ++y) {
        const std::size_t actual_row =
            static_cast<std::size_t>(y) * actual_row_floats;
        const std::size_t expected_row =
            static_cast<std::size_t>(y) * expected_row_floats;
        for (std::size_t x = 0U; x < active_row; ++x) {
            const double reference = expected.samples[expected_row + x];
            const double difference = std::abs(
                static_cast<double>(actual.samples[actual_row + x]) - reference
            );
            maximum_error = std::max(maximum_error, difference);
            if (difference
                > relative_tolerance * std::max(1.0, std::abs(reference))) {
                close = false;
            }
        }
    }
    return close;
}

[[nodiscard]] Rgb8Difference rgb8_difference(
    const std::span<const std::uint8_t> actual,
    const std::span<const std::uint8_t> expected
) {
    if (actual.size() != expected.size() || actual.empty()) {
        return {.maximum = 255U, .p99 = 255U};
    }
    std::vector<std::uint8_t> differences(actual.size());
    for (std::size_t index = 0U; index < actual.size(); ++index) {
        differences[index] = static_cast<std::uint8_t>(
            std::abs(
                static_cast<int>(actual[index])
                - static_cast<int>(expected[index])
            )
        );
    }
    const auto maximum = *std::ranges::max_element(differences);
    const std::size_t p99_index =
        static_cast<std::size_t>(0.99 * static_cast<double>(differences.size() - 1U));
    std::ranges::nth_element(
        differences,
        differences.begin() + static_cast<std::ptrdiff_t>(p99_index)
    );
    return {
        .maximum = maximum,
        .p99 = differences[p99_index],
    };
}

} // namespace shadow::image::warm_edit_gpu_contract::parity_fixture
