#include <shadow/image/edit.hpp>

#include "edit_contract_test_support.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void cube_lut_is_exactly_bypassable_and_blends_deterministically() {
  constexpr std::string_view identity_cube = R"cube(
LUT_3D_SIZE 2
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)cube";
  auto lut = image::parse_cube_lut(identity_cube);
  for (auto &entry : lut.entries) {
    entry[0] = 1.0F - entry[0];
  }
  const auto input = rgb_image(1, {0.25F, 0.5F, 0.75F});
  const image::AdjustmentNode half{
      .node_id = "lut-half",
      .parameters =
          image::CubeLutAdjustment{
              .lut = lut,
              .intensity = 0.5,
          },
  };
  const auto output =
      image::execute_adjustment_nodes(input, std::span{&half, 1U});
  expect_close(output.samples[0], 0.5F, "LUT intensity blends sampled red");
  expect_close(output.samples[1], 0.5F, "LUT preserves sampled green");
  expect_close(output.samples[2], 0.75F, "LUT preserves sampled blue");

  const image::AdjustmentNode empty_bypass{
      .node_id = "lut-empty-bypass",
      .parameters = image::CubeLutAdjustment{},
  };
  const auto bypass =
      image::execute_adjustment_nodes(input, std::span{&empty_bypass, 1U});
  expect(bypass.samples == input.samples,
         "an unselected zero-strength LUT is bit-exact");

  const image::AdjustmentNode missing_active{
      .node_id = "lut-missing-active",
      .parameters = image::CubeLutAdjustment{.intensity = 0.5},
  };
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(
            input, std::span{&missing_active, 1U}));
      },
      image::EditErrorCode::invalid_parameter, 0U,
      "an active LUT requires valid cube data");
}

void cube_lut_blending_preserves_finite_extreme_scene_values() {
  const float maximum = std::numeric_limits<float>::max();
  image::CubeLut3D lut{
      .size = 2U,
      .entries = std::vector<std::array<float, 3>>(
          8U, std::array<float, 3>{maximum, -maximum, 0.0F}),
  };
  const auto input = rgb_image(1, {-maximum, maximum, maximum});
  const std::array node{
      image::AdjustmentNode{
          .node_id = "lut-finite-extreme-blend",
          .parameters =
              image::CubeLutAdjustment{
                  .lut = std::move(lut),
                  .intensity = 0.5,
              },
      },
  };
  const auto output = image::execute_adjustment_nodes(input, node);
  expect(std::isfinite(output.samples[0]) && output.samples[0] == 0.0F,
         "LUT blending cancels opposing finite red extremes without float "
         "overflow");
  expect(std::isfinite(output.samples[1]) && output.samples[1] == 0.0F,
         "LUT blending cancels opposing finite green extremes without float "
         "overflow");
  expect(std::isfinite(output.samples[2]) &&
             output.samples[2] == maximum * 0.5F,
         "LUT blending keeps a finite half-strength super-white value");
}

} // namespace

int main() {
  cube_lut_is_exactly_bypassable_and_blends_deterministically();
  cube_lut_blending_preserves_finite_extreme_scene_values();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
