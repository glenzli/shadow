#include "working_color_math.hpp"

#include "adjustment_node_diagnostics.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>

namespace shadow::image::detail {

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr double d65_x = 0.3127;
constexpr double d65_y = 0.3290;

[[nodiscard]] std::optional<Matrix3> inverse(const Matrix3& matrix) noexcept {
    const double determinant =
        matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) -
        matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
        matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
        return std::nullopt;
    }

    const double reciprocal = 1.0 / determinant;
    Matrix3 result{{
        {{
            (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) * reciprocal,
            (matrix[0][2] * matrix[2][1] - matrix[0][1] * matrix[2][2]) * reciprocal,
            (matrix[0][1] * matrix[1][2] - matrix[0][2] * matrix[1][1]) * reciprocal,
        }},
        {{
            (matrix[1][2] * matrix[2][0] - matrix[1][0] * matrix[2][2]) * reciprocal,
            (matrix[0][0] * matrix[2][2] - matrix[0][2] * matrix[2][0]) * reciprocal,
            (matrix[0][2] * matrix[1][0] - matrix[0][0] * matrix[1][2]) * reciprocal,
        }},
        {{
            (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]) * reciprocal,
            (matrix[0][1] * matrix[2][0] - matrix[0][0] * matrix[2][1]) * reciprocal,
            (matrix[0][0] * matrix[1][1] - matrix[0][1] * matrix[1][0]) * reciprocal,
        }},
    }};
    if (!std::ranges::all_of(result, [](const Vector3& row) {
            return std::ranges::all_of(row,
                                       [](const double value) { return std::isfinite(value); });
        })) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] Chromaticity black_body_xy(const double kelvin) noexcept {
    const double temperature = std::clamp(kelvin, 1'667.0, 25'000.0);
    const double inverse = 1.0 / temperature;
    const double inverse2 = inverse * inverse;
    const double inverse3 = inverse2 * inverse;
    const double x =
        temperature <= 4'000.0
            ? -0.2661239e9 * inverse3 - 0.2343580e6 * inverse2 + 0.8776956e3 * inverse + 0.179910
            : -3.0258469e9 * inverse3 + 2.1070379e6 * inverse2 + 0.2226347e3 * inverse + 0.240390;
    double y = 0.0;
    if (temperature <= 2'222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    } else if (temperature <= 4'000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }
    return Chromaticity{.x = x, .y = y};
}

[[nodiscard]] Chromaticity tinted_white_xy(const double temperature, const double tint) noexcept {
    constexpr double d65_kelvin = 6'504.0;
    constexpr double d65_mired = 1'000'000.0 / d65_kelvin;
    constexpr double maximum_mired_shift = 80.0;
    const double target_mired = d65_mired + maximum_mired_shift * temperature;
    const Chromaticity locus = black_body_xy(1'000'000.0 / target_mired);

    // CIE 1960 UCS is used only to express a bounded green↔magenta offset.
    // Positive UI tint means magenta, hence the negative v displacement.
    const double denominator = -2.0 * locus.x + 12.0 * locus.y + 3.0;
    const double u = 4.0 * locus.x / denominator;
    const double v = 6.0 * locus.y / denominator - 0.025 * tint;
    const double inverse_denominator = 6.0 * u - 24.0 * v + 12.0;
    return Chromaticity{
        .x = 9.0 * u / inverse_denominator,
        .y = 6.0 * v / inverse_denominator,
    };
}

[[nodiscard]] Matrix3 multiply_matrices(const Matrix3& left, const Matrix3& right) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < rgb_channels; ++row) {
        for (std::size_t column = 0U; column < rgb_channels; ++column) {
            for (std::size_t inner = 0U; inner < rgb_channels; ++inner) {
                result[row][column] += left[row][inner] * right[inner][column];
            }
        }
    }
    return result;
}

} // namespace

bool finite_chromaticity(const Chromaticity& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

Vector3 apply_color_matrix(const Matrix3& matrix, const Vector3& vector) noexcept {
    return {
        matrix[0][0] * vector[0] + matrix[0][1] * vector[1] + matrix[0][2] * vector[2],
        matrix[1][0] * vector[0] + matrix[1][1] * vector[1] + matrix[1][2] * vector[2],
        matrix[2][0] * vector[0] + matrix[2][1] * vector[1] + matrix[2][2] * vector[2],
    };
}

WorkingSpaceTransform prepare_working_space_transform(const WorkingRgbSpace& space,
                                                      const AdjustmentNode& node,
                                                      const std::size_t index) {
    constexpr double white_tolerance = 5.0e-4;
    if (std::abs(space.white_point.x - d65_x) > white_tolerance ||
        std::abs(space.white_point.y - d65_y) > white_tolerance) {
        throw_node_error(EditErrorCode::invalid_working_space, index, node,
                         "D65 color adjustment requires a D65 RGB working space");
    }

    Matrix3 primary_matrix{};
    for (std::size_t primary = 0U; primary < rgb_channels; ++primary) {
        const Chromaticity xy = space.primaries[primary];
        if (xy.x < 0.0 || xy.y <= 0.0 || xy.x + xy.y > 1.0 + 1.0e-9) {
            throw_node_error(EditErrorCode::invalid_working_space, index, node,
                             "D65 color adjustment requires valid RGB primary chromaticities");
        }
        primary_matrix[0][primary] = xy.x / xy.y;
        primary_matrix[1][primary] = 1.0;
        primary_matrix[2][primary] = (1.0 - xy.x - xy.y) / xy.y;
    }

    const auto primary_inverse = inverse(primary_matrix);
    if (!primary_inverse.has_value()) {
        throw_node_error(EditErrorCode::invalid_working_space, index, node,
                         "D65 color adjustment requires independent RGB primaries");
    }
    const Vector3 white_xyz{
        space.white_point.x / space.white_point.y,
        1.0,
        (1.0 - space.white_point.x - space.white_point.y) / space.white_point.y,
    };
    const Vector3 primary_scales = apply_color_matrix(*primary_inverse, white_xyz);

    Matrix3 rgb_to_xyz{};
    for (std::size_t row = 0U; row < rgb_channels; ++row) {
        for (std::size_t column = 0U; column < rgb_channels; ++column) {
            rgb_to_xyz[row][column] = primary_matrix[row][column] * primary_scales[column];
        }
    }
    const auto xyz_to_rgb = inverse(rgb_to_xyz);
    if (!xyz_to_rgb.has_value()) {
        throw_node_error(EditErrorCode::invalid_working_space, index, node,
                         "D65 color adjustment could not derive an invertible RGB matrix");
    }

    // The declared coefficients are also used by selective tone and saturation. Reject a
    // contradictory space instead of silently using two different luminance definitions.
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        if (std::abs(rgb_to_xyz[1][channel] - space.luminance_coefficients[channel]) > 5.0e-4) {
            throw_node_error(
                EditErrorCode::invalid_working_space, index, node,
                "working-space luminance coefficients disagree with its RGB primaries");
        }
    }
    return WorkingSpaceTransform{.rgb_to_xyz = rgb_to_xyz, .xyz_to_rgb = *xyz_to_rgb};
}

Matrix3 prepare_rgb_white_balance_matrix(const WorkingRgbSpace& space,
                                         const RgbWhiteBalanceAdjustment& parameters,
                                         const AdjustmentNode& node, const std::size_t index) {
    const WorkingSpaceTransform working = prepare_working_space_transform(space, node, index);
    const Chromaticity target_xy = tinted_white_xy(parameters.temperature, parameters.tint);
    if (!finite_chromaticity(target_xy) || target_xy.x <= 0.0 || target_xy.y <= 0.0 ||
        target_xy.x + target_xy.y >= 1.0) {
        throw_node_error(EditErrorCode::invalid_parameter, index, node,
                         "RGB white balance produced an invalid target white");
    }

    constexpr Matrix3 cat16{{
        {{0.401288, 0.650173, -0.051461}},
        {{-0.250268, 1.204414, 0.045854}},
        {{-0.002079, 0.048952, 0.953127}},
    }};
    const auto cat16_inverse = inverse(cat16);
    if (!cat16_inverse.has_value()) {
        throw_node_error(EditErrorCode::numeric_overflow, index, node,
                         "CAT16 matrix is not invertible");
    }
    const Vector3 source_white_xyz{
        space.white_point.x / space.white_point.y,
        1.0,
        (1.0 - space.white_point.x - space.white_point.y) / space.white_point.y,
    };
    const Vector3 target_white_xyz{
        target_xy.x / target_xy.y,
        1.0,
        (1.0 - target_xy.x - target_xy.y) / target_xy.y,
    };
    const Vector3 source_response = apply_color_matrix(cat16, source_white_xyz);
    const Vector3 target_response = apply_color_matrix(cat16, target_white_xyz);
    Matrix3 response_scale{};
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        if (!std::isfinite(source_response[channel]) ||
            std::abs(source_response[channel]) <= 1.0e-12 ||
            !std::isfinite(target_response[channel])) {
            throw_node_error(EditErrorCode::numeric_overflow, index, node,
                             "CAT16 white response is not finite");
        }
        response_scale[channel][channel] = target_response[channel] / source_response[channel];
    }
    const Matrix3 xyz_adaptation =
        multiply_matrices(*cat16_inverse, multiply_matrices(response_scale, cat16));
    return multiply_matrices(working.xyz_to_rgb,
                             multiply_matrices(xyz_adaptation, working.rgb_to_xyz));
}

namespace {

[[nodiscard]] Vector3 xyz_to_oklab(const Vector3& xyz) noexcept {
    const double l = std::cbrt(0.8190224379967030 * xyz[0] + 0.3619062600528904 * xyz[1] -
                               0.1288737815209879 * xyz[2]);
    const double m = std::cbrt(0.0329836539323885 * xyz[0] + 0.9292868615863434 * xyz[1] +
                               0.0361446663506424 * xyz[2]);
    const double s = std::cbrt(0.0481771893596242 * xyz[0] + 0.2642395317527308 * xyz[1] +
                               0.6335478284694309 * xyz[2]);
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

[[nodiscard]] Vector3 oklab_to_xyz(const Vector3& lab) noexcept {
    const double l_root = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    const double m_root = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    const double s_root = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    return {
        1.2268798758459240 * l - 0.5578149944602170 * m + 0.2813910456659646 * s,
        -0.0405757452148009 * l + 1.1122868032803173 * m - 0.0717110580655164 * s,
        -0.0763729366746600 * l - 0.4214933324022431 * m + 1.5869240198367816 * s,
    };
}

} // namespace

Vector3 working_rgb_to_oklab(const WorkingSpaceTransform& transform, const Vector3& rgb) noexcept {
    return xyz_to_oklab(apply_color_matrix(transform.rgb_to_xyz, rgb));
}

Vector3 oklab_to_working_rgb(const WorkingSpaceTransform& transform, const Vector3& lab) noexcept {
    return apply_color_matrix(transform.xyz_to_rgb, oklab_to_xyz(lab));
}

} // namespace shadow::image::detail
