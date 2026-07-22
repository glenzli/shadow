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
    malformed_documents_fail_closed();
    std::cout << "shadow image LUT contract tests passed\n";
}
