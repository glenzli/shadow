#pragma once

#include "../edit/working_color_math.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace shadow::image::detail {

// Converts the validated double-precision working-space matrix into the
// float4-row ABI shared by resident Metal stages. The fourth lane remains
// reserved and zero so C++ and MSL retain one auditable record layout.
[[nodiscard]] inline bool fill_warm_color_matrix_rows(
    const Matrix3& matrix,
    std::array<float, 4U>& row_0,
    std::array<float, 4U>& row_1,
    std::array<float, 4U>& row_2
) noexcept {
    std::array<std::array<float, 4U>*, 3U> rows{&row_0, &row_1, &row_2};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            const float converted = static_cast<float>(matrix[row][column]);
            if (!std::isfinite(converted)) {
                return false;
            }
            (*rows[row])[column] = converted;
        }
    }
    return true;
}

} // namespace shadow::image::detail
