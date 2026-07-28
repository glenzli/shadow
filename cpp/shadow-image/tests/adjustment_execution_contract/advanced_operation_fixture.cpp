#include "advanced_operation_fixture.hpp"

#include <cstdint>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract::advanced_fixture {

[[nodiscard]] image::CubeLut3D advanced_test_lut() {
    image::CubeLut3D lut{
        .title = "GPU advanced contract",
        .size = 3U,
        .domain_min = {-0.25, -0.10, -0.20},
        .domain_max = {1.50, 1.30, 1.70},
    };
    lut.entries.reserve(27U);
    for (std::uint16_t blue = 0U; blue < lut.size; ++blue) {
        for (std::uint16_t green = 0U; green < lut.size; ++green) {
            for (std::uint16_t red = 0U; red < lut.size; ++red) {
                const float r = static_cast<float>(red) / 2.0F;
                const float g = static_cast<float>(green) / 2.0F;
                const float b = static_cast<float>(blue) / 2.0F;
                lut.entries.push_back({
                    0.05F + 0.78F * r + 0.12F * g,
                    0.03F + 0.82F * g + 0.10F * b,
                    0.02F + 0.14F * r + 0.76F * b,
                });
            }
        }
    }
    return lut;
}

} // namespace shadow::image::adjustment_execution_contract::advanced_fixture
