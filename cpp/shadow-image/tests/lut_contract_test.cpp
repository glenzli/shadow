#include <shadow/image/lut.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace image = shadow::image;

namespace {

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_close(const float actual, const float expected, const std::string_view message) {
    expect(std::abs(actual - expected) < 1.0e-6F, message);
}

constexpr std::string_view identity_lut = R"cube(
# red varies fastest in .cube storage
TITLE "Identity 2"
LUT_3D_SIZE 2
DOMAIN_MIN 0.0 0.0 0.0
DOMAIN_MAX 1.0 1.0 1.0
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)cube";

void identity_and_interpolation_are_explicit() {
    const auto lut = image::parse_cube_lut(identity_lut);
    expect(lut.title == "Identity 2", "TITLE is preserved");
    expect(lut.size == 2U && lut.entries.size() == 8U, "3D shape is validated");
    const auto midpoint = image::sample_cube_lut(lut, {0.25F, 0.5F, 0.75F});
    expect_close(midpoint[0], 0.25F, "trilinear red interpolation");
    expect_close(midpoint[1], 0.5F, "trilinear green interpolation");
    expect_close(midpoint[2], 0.75F, "trilinear blue interpolation");
    const auto clamped = image::sample_cube_lut(lut, {-4.0F, 0.5F, 8.0F});
    expect_close(clamped[0], 0.0F, "domain clamps low input");
    expect_close(clamped[2], 1.0F, "domain clamps high input");
}

void asymmetric_domains_map_boundaries_per_channel() {
    constexpr std::string_view asymmetric_domain_cube = R"cube(
TITLE "Asymmetric domain"
LUT_3D_SIZE 2
DOMAIN_MIN -1.0 -2.0 10.0
DOMAIN_MAX 3.0 2.0 20.0
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)cube";
    const auto lut = image::parse_cube_lut(asymmetric_domain_cube);
    const auto interior = image::sample_cube_lut(lut, {0.0F, 0.0F, 15.0F});
    expect_close(interior[0], 0.25F, "red uses its own asymmetric LUT domain");
    expect_close(interior[1], 0.5F, "green uses its own asymmetric LUT domain");
    expect_close(interior[2], 0.5F, "blue uses its own asymmetric LUT domain");

    const auto minimum = image::sample_cube_lut(lut, {-1.0F, -2.0F, 10.0F});
    expect_close(minimum[0], 0.0F, "the exact red domain minimum maps to the first cell");
    expect_close(minimum[1], 0.0F, "the exact green domain minimum maps to the first cell");
    expect_close(minimum[2], 0.0F, "the exact blue domain minimum maps to the first cell");

    const auto maximum = image::sample_cube_lut(lut, {3.0F, 2.0F, 20.0F});
    expect_close(maximum[0], 1.0F, "the exact red domain maximum maps to the last cell");
    expect_close(maximum[1], 1.0F, "the exact green domain maximum maps to the last cell");
    expect_close(maximum[2], 1.0F, "the exact blue domain maximum maps to the last cell");

    const auto clamped = image::sample_cube_lut(lut, {-8.0F, 7.0F, 25.0F});
    expect_close(clamped[0], 0.0F, "red clamps below its custom domain");
    expect_close(clamped[1], 1.0F, "green clamps above its custom domain");
    expect_close(clamped[2], 1.0F, "blue clamps above its custom domain");
}

void malformed_documents_fail_closed() {
    for (const auto source : {
             std::string_view{"LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"},
             std::string_view{"LUT_3D_SIZE 2\n0 0 0\n"},
             std::string_view{"LUT_3D_SIZE 2\nDOMAIN_MIN 1 0 0\nDOMAIN_MAX 1 1 1\n"},
             std::string_view{"UNKNOWN 2\n"},
         }) {
        try {
            static_cast<void>(image::parse_cube_lut(source));
            expect(false, "malformed .cube must fail");
        } catch (const std::invalid_argument&) {
        }
    }
}

} // namespace

int main() {
    identity_and_interpolation_are_explicit();
    asymmetric_domains_map_boundaries_per_channel();
    malformed_documents_fail_closed();
    std::cout << "shadow image LUT contract tests passed\n";
}
