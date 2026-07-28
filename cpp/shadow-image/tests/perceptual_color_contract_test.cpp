#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"
#include "perceptual_color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void perceptual_color_bypasses_independent_neutral_stages_exactly() {
    auto input = rgb_image(3, {
                                  0.70F,
                                  0.20F,
                                  0.10F,
                                  0.08F,
                                  0.45F,
                                  0.75F,
                                  1.20F,
                                  0.65F,
                                  0.25F,
                              });
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment mapping_only;
    mapping_only.saturation.fill(0.20);
    image::PerceptualColorAdjustment mapping_with_inert_selective = mapping_only;
    // Relative/absolute mode and lightness protection have no meaning until at
    // least one Selective Color CMYK component is non-zero.
    mapping_with_inert_selective.selective_color_relative = false;
    mapping_with_inert_selective.selective_color_lightness_protection = 1.0;
    const auto mapping_node = [](const std::string_view id,
                                 const image::PerceptualColorAdjustment& parameters) {
        return image::AdjustmentNode{
            .node_id = std::string(id),
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        };
    };
    const std::array mapping_only_nodes{
        mapping_node("mapping-only", mapping_only),
    };
    const std::array mapping_with_inert_selective_nodes{
        mapping_node("mapping-with-inert-selective", mapping_with_inert_selective),
    };
    const auto mapped = image::execute_adjustment_nodes(input, mapping_only_nodes);
    const auto mapped_with_inert_selective =
        image::execute_adjustment_nodes(input, mapping_with_inert_selective_nodes);
    expect(mapped.samples == mapped_with_inert_selective.samples,
           "neutral Selective Color is a bit-exact bypass inside active perceptual mapping");
    expect(mapped.samples != input.samples,
           "active perceptual mapping remains observable when Selective Color is neutral");

    image::PerceptualColorAdjustment selective_only;
    selective_only.selective_color_cmyk[0][1] = 0.25;
    image::PerceptualColorAdjustment selective_with_inert_ranges = selective_only;
    selective_with_inert_ranges.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 35.0,
        .width_degrees = 20.0,
        .softness = 0.5,
    };
    selective_with_inert_ranges.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 220.0,
        .width_degrees = 30.0,
        .softness = 0.5,
    });
    const std::array selective_only_nodes{
        mapping_node("selective-only", selective_only),
    };
    const std::array selective_with_inert_ranges_nodes{
        mapping_node("selective-with-inert-ranges", selective_with_inert_ranges),
    };
    const auto selected = image::execute_adjustment_nodes(input, selective_only_nodes);
    const auto selected_with_inert_ranges =
        image::execute_adjustment_nodes(input, selective_with_inert_ranges_nodes);
    expect(selected.samples == selected_with_inert_ranges.samples,
           "neutral Point Color ranges are a bit-exact bypass inside active Selective Color");
    expect(selected.samples != input.samples && selected.samples[1] < input.samples[1],
           "non-neutral Selective Color remains effective when perceptual mapping is neutral");

    image::PerceptualColorAdjustment opponent_only;
    opponent_only.global_a_balance = 0.24;
    image::PerceptualColorAdjustment opponent_with_inert_ranges = opponent_only;
    opponent_with_inert_ranges.color_range = selective_with_inert_ranges.color_range;
    opponent_with_inert_ranges.additional_color_ranges =
        selective_with_inert_ranges.additional_color_ranges;
    const auto balanced = image::execute_adjustment_nodes(
        input, std::array{mapping_node("opponent-only", opponent_only)});
    const auto balanced_with_inert_ranges = image::execute_adjustment_nodes(
        input, std::array{mapping_node("opponent-with-inert-ranges", opponent_with_inert_ranges)});
    expect(balanced.samples == balanced_with_inert_ranges.samples,
           "neutral Point Color ranges are a bit-exact bypass inside active "
           "opponent balance");
}

void perceptual_color_is_exactly_neutral_for_identity_and_low_chroma() {
    auto input = rgb_image(2, {0.25F, 0.25F, 0.25F, 0.5F, 0.50000006F, 0.5F});
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment neutral_parameters;
    neutral_parameters.color_range.enabled = true;
    neutral_parameters.color_range.center_degrees = 360.0;
    neutral_parameters.color_range.width_degrees = 1.0;
    neutral_parameters.color_range.softness = 0.0;
    neutral_parameters.selective_color_relative = false;
    neutral_parameters.selective_color_lightness_protection = 1.0;
    const std::array neutral_node{
        image::AdjustmentNode{
            .node_id = "neutral-perceptual-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = neutral_parameters,
        },
    };
    const auto neutral = image::execute_adjustment_nodes(input, neutral_node);
    expect(neutral.samples == input.samples,
           "neutral perceptual color is bit-exact even when its range selector is enabled");

    image::PerceptualColorAdjustment aggressive;
    aggressive.vibrance = 1.0;
    aggressive.hue.fill(1.0);
    aggressive.saturation.fill(1.0);
    aggressive.lightness.fill(1.0);
    aggressive.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 0.0,
        .width_degrees = 180.0,
        .softness = 1.0,
        .hue_shift_degrees = 180.0,
        .saturation = 1.0,
        .lightness = 1.0,
    };
    const std::array aggressive_node{
        image::AdjustmentNode{
            .node_id = "undefined-hue-guard",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = aggressive,
        },
    };
    const auto achromatic = image::execute_adjustment_nodes(input, aggressive_node);
    expect(achromatic.samples == input.samples,
           "gray and near-gray pixels do not acquire an arbitrary hue at low Oklch chroma");
}

void perceptual_color_preserves_extended_rec2020_range_and_exact_bypass() {
    auto rec2020_input = rgb_image(3, {
                                          -0.10F,
                                          0.25F,
                                          0.05F,
                                          2.50F,
                                          1.00F,
                                          0.30F,
                                          0.05F,
                                          1.70F,
                                          -0.03F,
                                      });
    image::PerceptualColorAdjustment parameters;
    parameters.vibrance = 0.4;
    parameters.hue.fill(0.15);
    parameters.saturation.fill(0.10);
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 180.0,
        .width_degrees = 180.0,
        .softness = 0.25,
        .lightness = 0.1,
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "rec2020-perceptual-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        },
    };
    const auto output = image::execute_adjustment_nodes(rec2020_input, nodes);
    expect(
        std::ranges::all_of(output.samples, [](const float value) { return std::isfinite(value); }),
        "primaries-derived D65 conversion supports finite Rec.2020 pixels");
    expect(std::ranges::any_of(output.samples, [](const float value) { return value < 0.0F; }) &&
               std::ranges::any_of(output.samples, [](const float value) { return value > 1.0F; }),
           "perceptual color retains negative and super-white scene-linear Rec.2020 values");

    image::PerceptualColorAdjustment neutral;
    neutral.color_range.enabled = true;
    neutral.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 300.0,
        .width_degrees = 10.0,
        .softness = 1.0,
    });
    neutral.selective_color_relative = false;
    neutral.selective_color_lightness_protection = 1.0;
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "extended-range-neutral-bypass",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = neutral,
        },
    };
    const auto bypassed = image::execute_adjustment_nodes(rec2020_input, neutral_nodes);
    expect(bypassed.samples == rec2020_input.samples,
           "neutral perceptual color is a bit-exact bypass for negative and super-white Rec.2020");

    auto super_white = rgb_image(1, {2.0F, 0.50F, 0.20F});
    image::PerceptualColorAdjustment selective;
    selective.selective_color_relative = false;
    for (std::size_t target = 0U; target < 6U; ++target) {
        selective.selective_color_cmyk[target][3] = 0.10;
    }
    const std::array selective_nodes{
        image::AdjustmentNode{
            .node_id = "super-white-selective-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = selective,
        },
    };
    const auto selected = image::execute_adjustment_nodes(super_white, selective_nodes);
    expect(
        std::ranges::all_of(selected.samples,
                            [](const float value) { return std::isfinite(value); }) &&
            std::ranges::any_of(selected.samples, [](const float value) { return value > 1.0F; }),
        "Selective Color normalizes around the scene peak without clipping super-white Rec.2020");
}

void perceptual_color_stage_classifier_matches_independent_controls() {
    image::PerceptualColorAdjustment neutral;
    const auto neutral_stages = image::detail::classify_perceptual_color(neutral);
    expect(neutral_stages.neutral(), "default Perceptual Color has no active internal stage");
    expect(neutral_stages.active_stage_count() == 0U,
           "the shared classifier reports zero emitted operations for an identity");

    image::PerceptualColorAdjustment hue_mapping;
    hue_mapping.saturation[2U] = 0.25;
    const auto hue_stages = image::detail::classify_perceptual_color(hue_mapping);
    expect(hue_stages.hue_mapping_active() && !hue_stages.opponent_balance_active() &&
               !hue_stages.selective_color_active(),
           "a color-mixer control activates only hue mapping");

    image::PerceptualColorAdjustment opponent_balance;
    opponent_balance.global_b_balance = -0.2;
    const auto opponent_stages = image::detail::classify_perceptual_color(opponent_balance);
    expect(!opponent_stages.hue_mapping_active() && opponent_stages.opponent_balance_active() &&
               !opponent_stages.selective_color_active(),
           "global opponent balance activates only its own stage");

    image::PerceptualColorAdjustment selective_color;
    selective_color.selective_color_cmyk[4U][1U] = 0.15;
    const auto selective_stages = image::detail::classify_perceptual_color(selective_color);
    expect(!selective_stages.hue_mapping_active() && !selective_stages.opponent_balance_active() &&
               selective_stages.selective_color_active(),
           "a CMYK target activates only Selective Color");

    image::PerceptualColorAdjustment inert_metadata;
    inert_metadata.color_range.enabled = true;
    inert_metadata.color_range.center_degrees = 180.0;
    inert_metadata.color_range.width_degrees = 20.0;
    inert_metadata.selective_color_relative = false;
    inert_metadata.selective_color_lightness_protection = 1.0;
    expect(image::detail::classify_perceptual_color(inert_metadata).neutral(),
           "selection geometry and mode metadata do not activate an inert stage");

    image::PerceptualColorAdjustment combined = hue_mapping;
    combined.global_a_balance = 0.1;
    combined.selective_color_cmyk[7U][3U] = -0.12;
    const auto combined_stages = image::detail::classify_perceptual_color(combined);
    expect(combined_stages.hue_mapping_active() && combined_stages.opponent_balance_active() &&
               combined_stages.selective_color_active() &&
               combined_stages.oklab_pipeline_active() &&
               combined_stages.active_stage_count() == 3U && !combined_stages.neutral(),
           "one classifier preserves all three active stages for CPU and Metal");

    const auto anchors = image::detail::perceptual_color_hue_anchors();
    expect(anchors.size() == image::perceptual_hue_band_count,
           "the Metal lowering view exposes every authored hue anchor");
    expect_close_double(anchors.front(), 29.23388536933038, 1.0e-12,
                        "the red Oklch anchor remains an authored semantic value");
    expect_close_double(anchors.back(), 328.36341829329797, 1.0e-12,
                        "the magenta Oklch anchor remains an authored semantic value");
}

} // namespace

int main() {
    perceptual_color_bypasses_independent_neutral_stages_exactly();
    perceptual_color_is_exactly_neutral_for_identity_and_low_chroma();
    perceptual_color_preserves_extended_rec2020_range_and_exact_bypass();
    perceptual_color_stage_classifier_matches_independent_controls();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
