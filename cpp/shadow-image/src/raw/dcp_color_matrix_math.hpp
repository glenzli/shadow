#pragma once

#include <shadow/image/dcp_color_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ranges>

namespace shadow::image::detail::dcp_color_matrix_math {

using Matrix3 = std::array<double, 9U>;
using Vector3 = std::array<double, 3U>;

inline constexpr Vector3 d50_xyz{0.964295676, 1.0, 0.825104603};
inline constexpr Vector3 d65_xyz{0.95047, 1.0, 1.08883};
inline constexpr Matrix3 bradford{
    0.8951, 0.2664, -0.1614,
    -0.7502, 1.7135, 0.0367,
    0.0389, -0.0685, 1.0296,
};
inline constexpr Matrix3 bradford_inverse{
    0.986992905, -0.147054256, 0.159962652,
    0.432305269, 0.518360272, 0.049291228,
    -0.008528665, 0.040042821, 0.968486696,
};
inline constexpr Matrix3 xyz_d65_to_linear_srgb{
    3.2404542, -1.5371385, -0.4985314,
    -0.9692660, 1.8760108, 0.0415560,
    0.0556434, -0.2040259, 1.0572252,
};
inline constexpr Matrix3 linear_srgb_to_xyz_d65{
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041,
};

[[nodiscard]] inline bool finite_matrix(const Matrix3& matrix) noexcept {
    return std::ranges::all_of(matrix, [](const double value) {
        return std::isfinite(value);
    });
}

[[nodiscard]] inline Matrix3 from_dcp(const DcpMatrix3x3& matrix) noexcept {
    return matrix.row_major;
}

[[nodiscard]] inline Vector3 multiply(
    const Matrix3& matrix,
    const Vector3& vector
) noexcept {
    Vector3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            result[row] += matrix[row * 3U + column] * vector[column];
        }
    }
    return result;
}

[[nodiscard]] inline Matrix3 multiply(
    const Matrix3& left,
    const Matrix3& right
) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] +=
                    left[row * 3U + inner] * right[inner * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] inline Matrix3 scale_matrix(const Matrix3& matrix, const double scale) noexcept {
    Matrix3 result = matrix;
    for (double& value : result) {
        value *= scale;
    }
    return result;
}

[[nodiscard]] inline Matrix3 interpolate(
    const Matrix3& first,
    const Matrix3& second,
    const double first_weight
) noexcept {
    Matrix3 result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] =
            first[index] * first_weight + second[index] * (1.0 - first_weight);
    }
    return result;
}

[[nodiscard]] inline Matrix3 invert(const Matrix3& matrix) {
    const double determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    double largest = 0.0;
    for (const double value : matrix) {
        largest = std::max(largest, std::abs(value));
    }
    const double scale = std::max(1.0, largest * largest * largest);
    if (!std::isfinite(determinant)
        || std::abs(determinant) <= std::numeric_limits<double>::epsilon() * scale * 64.0) {
        throw DcpColorDevelopmentError(
            DcpColorDevelopmentErrorCode::singular_matrix,
            "DCP color matrix is singular"
        );
    }
    const double reciprocal = 1.0 / determinant;
    return {
        (matrix[4] * matrix[8] - matrix[5] * matrix[7]) * reciprocal,
        (matrix[2] * matrix[7] - matrix[1] * matrix[8]) * reciprocal,
        (matrix[1] * matrix[5] - matrix[2] * matrix[4]) * reciprocal,
        (matrix[5] * matrix[6] - matrix[3] * matrix[8]) * reciprocal,
        (matrix[0] * matrix[8] - matrix[2] * matrix[6]) * reciprocal,
        (matrix[2] * matrix[3] - matrix[0] * matrix[5]) * reciprocal,
        (matrix[3] * matrix[7] - matrix[4] * matrix[6]) * reciprocal,
        (matrix[1] * matrix[6] - matrix[0] * matrix[7]) * reciprocal,
        (matrix[0] * matrix[4] - matrix[1] * matrix[3]) * reciprocal,
    };
}

[[nodiscard]] inline Matrix3 chromatic_adaptation(
    const Vector3& source_white,
    const Vector3& destination_white
) {
    const Vector3 source_cone = multiply(bradford, source_white);
    const Vector3 destination_cone = multiply(bradford, destination_white);
    Matrix3 scale{};
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        if (!std::isfinite(source_cone[channel]) || source_cone[channel] <= 0.0
            || !std::isfinite(destination_cone[channel])
            || destination_cone[channel] <= 0.0) {
            throw DcpColorDevelopmentError(
                DcpColorDevelopmentErrorCode::invalid_white_point,
                "DCP Bradford adaptation received a non-positive cone response"
            );
        }
        scale[channel * 3U + channel] = destination_cone[channel] / source_cone[channel];
    }
    return multiply(bradford_inverse, multiply(scale, bradford));
}

} // namespace shadow::image::detail::dcp_color_matrix_math
