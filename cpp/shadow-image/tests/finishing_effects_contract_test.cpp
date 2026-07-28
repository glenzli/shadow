#include "edit_contract_test_support.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void global_effect_coordinates_are_tile_invariant() {
    std::vector<float> full_samples;
    full_samples.reserve(4U * 3U * 3U);
    for (std::size_t index = 0; index < 12U; ++index) {
        const float value = 0.15F + static_cast<float>(index) * 0.025F;
        full_samples.insert(full_samples.end(), {value, value * 0.9F, value * 0.8F});
    }
    const auto full_input = rgb_raster(4, 3, full_samples);
    image::SharpenAdjustment parameters;
    parameters.grain_amount = 0.7;
    parameters.grain_size = 0.75;
    parameters.grain_roughness = 0.65;
    parameters.vignette_amount = -0.6;
    parameters.vignette_midpoint = 0.35;
    parameters.vignette_roundness = 0.25;
    parameters.execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "global-effects",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::finishing_effects_implementation_version,
            .parameters = parameters,
        },
    };
    const image::AdjustmentExecutionContext full_context{
        .full_dimensions = {4, 3},
    };
    const auto full_output = image::execute_adjustment_nodes(full_input, nodes, full_context);

    for (std::uint32_t tile_index = 0; tile_index < 2U; ++tile_index) {
        std::vector<float> tile_samples;
        tile_samples.reserve(2U * 3U * 3U);
        for (std::uint32_t y = 0; y < 3U; ++y) {
            const std::size_t source = (static_cast<std::size_t>(y) * 4U + tile_index * 2U) * 3U;
            tile_samples.insert(
                tile_samples.end(),
                full_samples.begin() + static_cast<std::ptrdiff_t>(source),
                full_samples.begin() + static_cast<std::ptrdiff_t>(source + 6U)
            );
        }
        const auto tile_input = rgb_raster(2, 3, std::move(tile_samples));
        const image::AdjustmentExecutionContext tile_context{
            .origin_x = tile_index * 2U,
            .origin_y = 0,
            .full_dimensions = {4, 3},
        };
        const auto tile_output = image::execute_adjustment_nodes(tile_input, nodes, tile_context);
        for (std::uint32_t y = 0; y < 3U; ++y) {
            for (std::uint32_t x = 0; x < 2U; ++x) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    const std::size_t tile_sample =
                        (static_cast<std::size_t>(y) * 2U + x) * 3U + channel;
                    const std::size_t full_sample =
                        (static_cast<std::size_t>(y) * 4U + tile_index * 2U + x) * 3U
                        + channel;
                    expect_close(
                        tile_output.samples[tile_sample],
                        full_output.samples[full_sample],
                        "grain and vignette remain identical across independently rendered tiles"
                    );
                }
            }
        }
    }
}

void neutral_finishing_pass_is_bit_exact() {
  const auto input =
      rgb_image(2U, {0.1F, 0.2F, 0.3F, 0.8F, 0.7F, 0.6F, 42.0F}, 1U);
  image::SharpenAdjustment parameters;
  parameters.execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
  const std::array nodes{
      image::AdjustmentNode{
          .node_id = "neutral-finishing",
          .parameter_schema_version = image::detail_effects_parameter_schema_version,
          .implementation_version = image::finishing_effects_implementation_version,
          .parameters = parameters,
      },
  };
  const auto output = image::execute_adjustment_nodes(input, nodes);
  expect(output.samples == input.samples,
         "neutral grain and vignette bypass the fused finishing traversal exactly");
}

void grain_is_deterministic_and_vignette_uses_full_raster_geometry() {
  const auto input = rgb_raster(
      5U, 5U,
      std::vector<float>(5U * 5U * 3U, 0.5F));

  image::SharpenAdjustment grain;
  grain.execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
  grain.grain_amount = 0.8;
  grain.grain_size = 0.7;
  grain.grain_roughness = 0.6;
  const std::array grain_nodes{
      image::AdjustmentNode{
          .node_id = "deterministic-grain",
          .parameter_schema_version = image::detail_effects_parameter_schema_version,
          .implementation_version = image::finishing_effects_implementation_version,
          .parameters = grain,
      },
  };
  const auto first = image::execute_adjustment_nodes(input, grain_nodes);
  const auto second = image::execute_adjustment_nodes(input, grain_nodes);
  expect(first.samples == second.samples && first.samples != input.samples,
         "coordinate grain is deterministic while remaining visibly active");

  image::SharpenAdjustment vignette;
  vignette.execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
  vignette.vignette_amount = -0.8;
  vignette.vignette_midpoint = 0.35;
  vignette.vignette_feather = 0.5;
  const std::array vignette_nodes{
      image::AdjustmentNode{
          .node_id = "full-raster-vignette",
          .parameter_schema_version = image::detail_effects_parameter_schema_version,
          .implementation_version = image::finishing_effects_implementation_version,
          .parameters = vignette,
      },
  };
  const auto shaded = image::execute_adjustment_nodes(input, vignette_nodes);
  constexpr std::size_t corner = 0U;
  constexpr std::size_t center = (2U * 5U + 2U) * 3U;
  expect(shaded.samples[corner] < shaded.samples[center],
         "negative vignette darkens full-raster corners more than the optical center");
}

} // namespace

int main() {
  global_effect_coordinates_are_tile_invariant();
  neutral_finishing_pass_is_bit_exact();
  grain_is_deterministic_and_vignette_uses_full_raster_geometry();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
