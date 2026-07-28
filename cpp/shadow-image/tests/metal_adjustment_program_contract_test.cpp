#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"
#include "metal_adjustment_program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

template <typename Parameters>
[[nodiscard]] image::AdjustmentNode adjustment_node(std::string id, Parameters parameters) {
    return image::AdjustmentNode{
        .node_id = std::move(id),
        .parameter_schema_version = image::adjustment_parameter_schema_version,
        .implementation_version = image::adjustment_implementation_version,
        .parameters = std::move(parameters),
    };
}

[[nodiscard]] image::CubeLutAdjustment two_by_two_lut() {
    image::CubeLutAdjustment parameters;
    parameters.intensity = 0.65;
    parameters.lut.size = 2U;
    parameters.lut.entries.reserve(8U);
    for (std::size_t blue = 0U; blue < 2U; ++blue) {
        for (std::size_t green = 0U; green < 2U; ++green) {
            for (std::size_t red = 0U; red < 2U; ++red) {
                parameters.lut.entries.push_back({
                    static_cast<float>(red),
                    static_cast<float>(green),
                    static_cast<float>(blue),
                });
            }
        }
    }
    return parameters;
}

[[nodiscard]] std::vector<image::AdjustmentNode> complete_program_nodes() {
    image::OklabLightnessToneCurve lightness;
    lightness.lightness.points = {{0.0, 0.0}, {0.5, 0.42}, {1.0, 1.0}};

    image::OklabOpponentToneCurves opponent_curves;
    opponent_curves.a.points = {{0.0, 0.0}, {0.5, 0.08}, {1.0, 0.0}};
    opponent_curves.b.points = {{0.0, 0.0}, {0.5, -0.06}, {1.0, 0.0}};

    image::OklabColorWarperAdjustment warper;
    warper.strength = 0.75;
    warper.control_points.front().a_offset = 0.02;
    warper.control_points.back().b_offset = -0.015;

    image::PerceptualColorAdjustment perceptual;
    perceptual.hue[0U] = 0.20;
    perceptual.global_a_balance = 0.15;
    perceptual.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 210.0,
        .width_degrees = 24.0,
        .softness = 0.40,
        .hue_shift_degrees = -8.0,
        .saturation = 0.12,
        .lightness = -0.05,
    });
    perceptual.selective_color_cmyk[0U][1U] = 0.18;

    image::PerceptualColorAdjustment second_perceptual;
    second_perceptual.saturation[3U] = -0.12;
    second_perceptual.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 40.0,
        .width_degrees = 18.0,
        .softness = 0.25,
        .hue_shift_degrees = 5.0,
    });
    second_perceptual.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 300.0,
        .width_degrees = 32.0,
        .softness = 0.60,
        .lightness = 0.08,
    });
    second_perceptual.selective_color_cmyk[4U][2U] = -0.14;

    std::vector<image::AdjustmentNode> nodes;
    nodes.push_back(adjustment_node("exposure", image::ExposureAdjustment{.stops = 0.5}));
    nodes.push_back(adjustment_node("lightness-curve", std::move(lightness)));
    nodes.push_back(adjustment_node("opponent-curves", std::move(opponent_curves)));
    nodes.push_back(adjustment_node("lut", two_by_two_lut()));
    nodes.push_back(adjustment_node("warper", std::move(warper)));
    nodes.push_back(adjustment_node("perceptual", std::move(perceptual)));
    nodes.push_back(adjustment_node("second-lut", two_by_two_lut()));
    nodes.push_back(adjustment_node("second-perceptual", std::move(second_perceptual)));
    return nodes;
}

[[nodiscard]] image::FloatRgbImage program_input() {
    auto input = rgb_image(2U, {0.18F, 0.25F, 0.40F, 0.72F, 0.34F, 0.12F});
    input.working_space = linear_srgb();
    return input;
}

void metal_program_compiles_one_consistent_multi_resource_transaction() {
    const auto nodes = complete_program_nodes();
    const auto input = program_input();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto preparation = image::detail::prepare_metal_adjustment(input, nodes, plan, {});
    expect(preparation.program.has_value(),
           std::string("the portable Metal host compiler accepts a complete pixel-local plan: ") +
               preparation.diagnostic);
    if (!preparation.program.has_value()) {
        return;
    }

    const auto& program = *preparation.program;
    constexpr std::array expected_opcodes{
        image::detail::MetalAdjustmentOpcode::exposure,
        image::detail::MetalAdjustmentOpcode::oklab_lightness_tone_curve,
        image::detail::MetalAdjustmentOpcode::oklab_opponent_tone_curves,
        image::detail::MetalAdjustmentOpcode::lut_3d,
        image::detail::MetalAdjustmentOpcode::oklab_color_warper,
        image::detail::MetalAdjustmentOpcode::perceptual_mapping,
        image::detail::MetalAdjustmentOpcode::oklab_opponent_balance,
        image::detail::MetalAdjustmentOpcode::selective_color,
        image::detail::MetalAdjustmentOpcode::lut_3d,
        image::detail::MetalAdjustmentOpcode::perceptual_mapping,
        image::detail::MetalAdjustmentOpcode::selective_color,
    };
    constexpr std::array<std::uint32_t, expected_opcodes.size()> expected_sources{
        0U, 1U, 2U, 3U, 4U, 5U, 5U, 5U, 6U, 7U, 7U,
    };
    expect(program.operations.size() == expected_opcodes.size() &&
               program.invocation.step_count == expected_opcodes.size(),
           "Perceptual Color nodes expand into adjacent Metal sub-operations");
    if (program.operations.size() != expected_opcodes.size()) {
        return;
    }
    for (std::size_t index = 0U; index < expected_opcodes.size(); ++index) {
        expect(program.operations[index].opcode ==
                       static_cast<std::uint32_t>(expected_opcodes[index]) &&
                   program.operations[index].source_node_index == expected_sources[index],
               "Metal lowering preserves complete operation and source-node order");
    }

    expect(program.invocation.curve_segment_count == 6U && program.curve_segments.size() == 6U,
           "the first pass reserves every lightness and opponent curve segment");
    expect(program.operations[1U].resource_offset == 0U &&
               program.operations[1U].resource_count == 2U &&
               program.operations[2U].resource_offset == 2U &&
               program.operations[2U].resource_count == 2U &&
               program.operations[2U].secondary_resource_offset == 4U &&
               program.operations[2U].secondary_resource_count == 2U,
           "curve offsets remain contiguous in lowering order");

    expect(program.invocation.lut_entry_count == 16U && program.lut_entries.size() == 16U &&
               program.operations[3U].resource_offset == 0U &&
               program.operations[3U].resource_count == 2U &&
               program.operations[8U].resource_offset == 8U &&
               program.operations[8U].resource_count == 2U,
           "LUT lowering distinguishes cube edge length from cumulative side-table offsets");

    constexpr std::uint32_t warper_entries =
        static_cast<std::uint32_t>(image::oklab_color_warper_control_point_count);
    constexpr std::uint32_t mixer_entries =
        static_cast<std::uint32_t>(image::perceptual_hue_band_count);
    expect(program.invocation.perceptual_mixer_entry_count == warper_entries + 2U * mixer_entries &&
               program.perceptual_mixer_entries.size() == warper_entries + 2U * mixer_entries &&
               program.operations[4U].resource_offset == 0U &&
               program.operations[4U].resource_count == warper_entries &&
               program.operations[5U].resource_offset == warper_entries &&
               program.operations[5U].resource_count == mixer_entries &&
               program.operations[9U].resource_offset == warper_entries + mixer_entries &&
               program.operations[9U].resource_count == mixer_entries,
           "Color Warper and Perceptual Color share one side table without overlapping ranges");

    expect(program.invocation.perceptual_range_entry_count == 3U &&
               program.perceptual_range_entries.size() == 3U &&
               program.operations[5U].secondary_resource_offset == 0U &&
               program.operations[5U].secondary_resource_count == 1U &&
               program.operations[9U].secondary_resource_offset == 1U &&
               program.operations[9U].secondary_resource_count == 2U,
           "ordered Point Color ranges use the mapping operation's secondary resource range");
    expect(program.invocation.selective_color_entry_count ==
                   2U * image::selective_color_target_count &&
               program.selective_color_entries.size() == 2U * image::selective_color_target_count &&
               program.operations[7U].resource_offset == 0U &&
               program.operations[7U].resource_count == image::selective_color_target_count &&
               program.operations[10U].resource_offset == image::selective_color_target_count &&
               program.operations[10U].resource_count == image::selective_color_target_count,
           "Selective Color reserves its complete fixed target table");
}

void metal_program_rejects_stale_or_non_pixel_local_plans_before_lowering() {
    const auto nodes = complete_program_nodes();
    const auto input = program_input();

    auto stale_plan = image::compile_edit_execution_plan(nodes);
    ++stale_plan.source_node_count;
    const auto stale = image::detail::prepare_metal_adjustment(input, nodes, stale_plan, {});
    expect(!stale.program.has_value() &&
               stale.diagnostic == "Metal adjustment plan no longer matches its source nodes",
           "the Metal compiler rejects a stale source-node count");

    auto mismatched_plan = image::compile_edit_execution_plan(nodes);
    mismatched_plan.segments.front().steps.front().operation =
        image::AdjustmentOperation::saturation;
    const auto mismatched =
        image::detail::prepare_metal_adjustment(input, nodes, mismatched_plan, {});
    expect(!mismatched.program.has_value() &&
               mismatched.diagnostic ==
                   "Metal adjustment plan operation no longer matches its source node",
           "the Metal compiler rejects operation registry drift before resource allocation");

    auto neighborhood_plan = image::compile_edit_execution_plan(nodes);
    neighborhood_plan.segments.front().locality = image::AdjustmentLocality::neighborhood;
    const auto neighborhood =
        image::detail::prepare_metal_adjustment(input, nodes, neighborhood_plan, {});
    expect(!neighborhood.program.has_value() &&
               neighborhood.diagnostic == "Metal adjustment cannot prepare a neighborhood segment",
           "the portable Metal compiler rejects neighborhood execution segments");
}

void metal_program_has_an_explicit_prevalidated_raster_contract() {
    const auto nodes = complete_program_nodes();
    auto metadata_only = program_input();
    const auto plan = image::compile_edit_execution_plan(nodes);
    metadata_only.samples.clear();

    expect_edit_error(
        [&] {
            static_cast<void>(
                image::detail::prepare_metal_adjustment(metadata_only, nodes, plan, {}));
        },
        image::EditErrorCode::invalid_image_layout, std::nullopt,
        "the default Metal preparation path validates complete raster storage");

    const auto resident =
        image::detail::prepare_metal_adjustment(metadata_only, nodes, plan, {}, true);
    expect(resident.program.has_value(),
           std::string("a resident source may reuse prior raster validation while retaining "
                       "metadata checks: ") +
               resident.diagnostic);
}

} // namespace

int main() {
    metal_program_compiles_one_consistent_multi_resource_transaction();
    metal_program_rejects_stale_or_non_pixel_local_plans_before_lowering();
    metal_program_has_an_explicit_prevalidated_raster_contract();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
