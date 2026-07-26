#pragma once

#include <shadow/image/edit.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image::test {

namespace image = shadow::image;

inline int failures = 0;

inline void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

inline void expect_close(const float actual, const float expected, const std::string_view message) {
    if (std::abs(actual - expected) > 1.0e-5F) {
        std::cerr << "FAILED: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        ++failures;
    }
}

inline void expect_close_double(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string_view message
) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAILED: " << message << " (actual=" << actual
                  << ", expected=" << expected << ", tolerance=" << tolerance << ")\n";
        ++failures;
    }
}

[[nodiscard]] inline image::WorkingRgbSpace linear_rec2020() {
    return image::WorkingRgbSpace{
        .id = "linear-rec2020-d65",
        .primaries = {{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2627, 0.6780, 0.0593},
    };
}

[[nodiscard]] inline image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] inline float linear_srgb_component_from_8_bit(const int value) {
    const double encoded = static_cast<double>(value) / 255.0;
    return static_cast<float>(
        encoded <= 0.04045
            ? encoded / 12.92
            : std::pow((encoded + 0.055) / 1.055, 2.4)
    );
}

[[nodiscard]] inline std::array<float, 3> linear_srgb_from_oklch(
    const double lightness,
    const double chroma,
    const double hue_degrees
) {
    constexpr double test_pi = 3.141592653589793238462643383279502884;
    const double hue = hue_degrees * test_pi / 180.0;
    const double a = chroma * std::cos(hue);
    const double b = chroma * std::sin(hue);
    const double l_root = lightness + 0.3963377774 * a + 0.2158037573 * b;
    const double m_root = lightness - 0.1055613458 * a - 0.0638541728 * b;
    const double s_root = lightness - 0.0894841775 * a - 1.2914855480 * b;
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    const std::array xyz{
        1.2268798758459240 * l - 0.5578149944602170 * m + 0.2813910456659646 * s,
        -0.0405757452148009 * l + 1.1122868032803173 * m - 0.0717110580655164 * s,
        -0.0763729366746600 * l - 0.4214933324022431 * m + 1.5869240198367816 * s,
    };
    return {
        static_cast<float>(
            3.240969941904521 * xyz[0] - 1.537383177570093 * xyz[1]
            - 0.498610760293000 * xyz[2]
        ),
        static_cast<float>(
            -0.969243636280880 * xyz[0] + 1.875967501507720 * xyz[1]
            + 0.041555057407175 * xyz[2]
        ),
        static_cast<float>(
            0.055630079696993 * xyz[0] - 0.203976958888970 * xyz[1]
            + 1.056971514242878 * xyz[2]
        ),
    };
}

[[nodiscard]] inline std::array<double, 3> oklab_from_linear_srgb(
    const std::array<float, 3>& rgb
) {
    const double l = std::cbrt(
        0.4122214708 * static_cast<double>(rgb[0])
        + 0.5363325363 * static_cast<double>(rgb[1])
        + 0.0514459929 * static_cast<double>(rgb[2])
    );
    const double m = std::cbrt(
        0.2119034982 * static_cast<double>(rgb[0])
        + 0.6806995451 * static_cast<double>(rgb[1])
        + 0.1073969566 * static_cast<double>(rgb[2])
    );
    const double s = std::cbrt(
        0.0883024619 * static_cast<double>(rgb[0])
        + 0.2817188376 * static_cast<double>(rgb[1])
        + 0.6299787005 * static_cast<double>(rgb[2])
    );
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

[[nodiscard]] inline image::FloatRgbImage rgb_image(
    const std::uint32_t width,
    std::vector<float> samples,
    const std::size_t padding_samples = 0U
) {
    const std::size_t stride = static_cast<std::size_t>(width) * 3U + padding_samples;
    return image::FloatRgbImage{
        .dimensions = {width, 1},
        .row_stride_bytes = stride * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_rec2020(),
        .samples = std::move(samples),
    };
}

[[nodiscard]] inline image::FloatRgbImage rgb_raster(
    const std::uint32_t width,
    const std::uint32_t height,
    std::vector<float> samples
) {
    return image::FloatRgbImage{
        .dimensions = {width, height},
        .row_stride_bytes = static_cast<std::size_t>(width) * 3U * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_rec2020(),
        .samples = std::move(samples),
    };
}

template <typename Function>
inline void expect_edit_error(
    Function&& function,
    const image::EditErrorCode expected_code,
    const std::optional<std::size_t> expected_node,
    const std::string_view message
) {
    try {
        function();
        expect(false, message);
    } catch (const image::EditError& error) {
        expect(error.code() == expected_code, message);
        expect(error.node_index() == expected_node, "edit error reports the failing node index");
    }
}

} // namespace shadow::image::test
