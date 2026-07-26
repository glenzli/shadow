#include "edit_contract_test_support.hpp"

#include <cstdlib>

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

void local_mask_layers_blend_complete_adjustments_in_global_coordinates() {
    const auto input = rgb_raster(
        4U,
        1U,
        {
            0.25F, 0.25F, 0.25F,
            0.25F, 0.25F, 0.25F,
            0.25F, 0.25F, 0.25F,
            0.25F, 0.25F, 0.25F,
        }
    );
    const image::AdjustmentNode exposure{
        .node_id = "local-mask-exposure",
        .parameters = image::ExposureAdjustment{.stops = 1.0},
    };
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "linear-layer",
            .mask = image::LocalMask{
                .kind = image::LocalMaskKind::linear_gradient,
                .x0 = 0.25,
                .y0 = 0.5,
                .x1 = 0.75,
                .y1 = 0.5,
            },
            .nodes = {exposure},
        },
    };
    const auto output = image::execute_adjustment_layers(input, layers);
    expect_close(output.samples[0], 0.25F, "linear masks leave the zero-coverage edge unchanged");
    expect_close(output.samples[3], 0.3125F, "linear masks blend an intermediate before/after result");
    expect_close(output.samples[6], 0.4375F, "linear masks continue their normalized ramp");
    expect_close(output.samples[9], 0.5F, "linear masks apply the complete adjustment at full coverage");

    const auto full = image::execute_adjustment_layers(
        input,
        layers,
        image::AdjustmentExecutionContext{.full_dimensions = {4U, 1U}}
    );
    for (std::uint32_t tile_index = 0U; tile_index < 2U; ++tile_index) {
        const std::size_t begin = static_cast<std::size_t>(tile_index) * 6U;
        const auto tile = rgb_raster(
            2U,
            1U,
            std::vector<float>(
                input.samples.begin() + static_cast<std::ptrdiff_t>(begin),
                input.samples.begin() + static_cast<std::ptrdiff_t>(begin + 6U)
            )
        );
        const auto rendered_tile = image::execute_adjustment_layers(
            tile,
            layers,
            image::AdjustmentExecutionContext{
                .origin_x = tile_index * 2U,
                .full_dimensions = {4U, 1U},
            }
        );
        for (std::size_t sample = 0U; sample < rendered_tile.samples.size(); ++sample) {
            expect_close(
                rendered_tile.samples[sample],
                full.samples[begin + sample],
                "local masks retain the same coverage for independently rendered detail tiles"
            );
        }
    }

    const std::array invalid_layers{
        image::AdjustmentLayer{
            .layer_id = "invalid-radial",
            .mask = image::LocalMask{
                .kind = image::LocalMaskKind::radial_gradient,
                .x0 = 0.5,
                .y0 = 0.5,
                .radius_x = 0.0,
                .radius_y = 0.2,
            },
            .nodes = {exposure},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_layers(input, invalid_layers)); },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "invalid local-mask geometry fails closed before it can affect a recipe"
    );

    const auto brush_input = rgb_raster(
        5U,
        3U,
        std::vector<float>(5U * 3U * 3U, 0.25F)
    );
    const std::array brush_layers{
        image::AdjustmentLayer{
            .layer_id = "brush-layer",
            .mask = image::LocalMask{
                .kind = image::LocalMaskKind::brush,
                .radius_x = 0.22,
                .feather = 0.0,
                .points = {{
                    .x = 0.5,
                    .y = 0.5,
                    .begins_stroke = true,
                }},
            },
            .nodes = {exposure},
        },
    };
    const auto brush_output = image::execute_adjustment_layers(brush_input, brush_layers);
    const std::size_t center_sample = (1U * 5U + 2U) * 3U;
    expect_close(
        brush_output.samples[center_sample],
        0.5F,
        "brush masks apply the complete Grade Node inside the painted radius"
    );
    expect_close(
        brush_output.samples[0],
        0.25F,
        "brush masks leave distant pixels unchanged"
    );
}

void spot_heal_repairs_small_defects_in_global_coordinates() {
    std::vector<float> samples(9U * 9U * 3U, 0.2F);
    const std::size_t center = (4U * 9U + 4U) * 3U;
    samples[center] = 1.0F;
    samples[center + 1U] = 0.0F;
    samples[center + 2U] = 0.0F;
    const auto input = rgb_raster(9U, 9U, samples);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "spot-heal-dust",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 0.5,
                    .center_y = 0.5,
                    .radius_level_zero_pixels = 1U,
                }},
            },
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    expect(
        plan.cumulative_footprint == image::AdjustmentFootprint{
            .horizontal_radius = 4U,
            .vertical_radius = 4U,
        },
        "spot-heal declares enough detail-tile support for its reconstruction ring"
    );
    const auto output = image::execute_adjustment_nodes(input, nodes);
    expect_close(output.samples[center], 0.2F, "spot-heal restores a red defect from its ring");
    expect_close(
        output.samples[center + 1U],
        0.2F,
        "spot-heal restores a green defect component from its ring"
    );
    expect_close(
        output.samples[center + 2U],
        0.2F,
        "spot-heal restores a blue defect component from its ring"
    );

    image::SpotHealAdjustment invalid;
    invalid.spots.push_back(image::SpotHealTarget{
        .center_x = 0.5,
        .center_y = 0.5,
        .radius_level_zero_pixels = 0U,
    });
    expect_edit_error(
        [&] { image::validate_spot_heal(invalid); },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "spot-heal rejects an out-of-contract radius before touching pixels"
    );

    std::vector<float> clone_samples(9U * 9U * 3U, 0.0F);
    for (std::uint32_t y = 0U; y < 9U; ++y) {
        for (std::uint32_t x = 0U; x < 9U; ++x) {
            const std::size_t sample = (static_cast<std::size_t>(y) * 9U + x) * 3U;
            clone_samples[sample] = static_cast<float>(x) / 10.0F;
            clone_samples[sample + 1U] = static_cast<float>(y) / 10.0F;
            clone_samples[sample + 2U] = 0.25F;
        }
    }
    const std::array clone_nodes{
        image::AdjustmentNode{
            .node_id = "spot-clone",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 0.5,
                    .center_y = 0.5,
                    .radius_level_zero_pixels = 1U,
                    .mode = image::SpotRepairMode::clone,
                    .source_offset_x_radii = 2.0,
                    .source_offset_y_radii = -1.0,
                    .feather = 0.0,
                }},
            },
        },
    };
    const auto cloned = image::execute_adjustment_nodes(
        rgb_raster(9U, 9U, clone_samples),
        clone_nodes
    );
    expect_close(
        cloned.samples[center],
        0.6F,
        "clone repair copies the selected nearby source into the target centre"
    );
    expect_close(
        cloned.samples[center + 1U],
        0.3F,
        "clone repair preserves the two-dimensional source offset"
    );
}

void continuous_retouch_strokes_sweep_one_connected_repair_region() {
    constexpr std::uint32_t width = 17U;
    constexpr std::uint32_t height = 9U;
    std::vector<float> clone_samples(static_cast<std::size_t>(width) * height * 3U, 0.0F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t sample = (static_cast<std::size_t>(y) * width + x) * 3U;
            clone_samples[sample] = static_cast<float>(x) / 20.0F;
            clone_samples[sample + 1U] = static_cast<float>(y) / 20.0F;
            clone_samples[sample + 2U] = 0.25F;
        }
    }
    const image::RetouchStroke clone_stroke{
        .points = {
            {.x = 4.5 / static_cast<double>(width), .y = 4.5 / static_cast<double>(height)},
            {.x = 12.5 / static_cast<double>(width), .y = 4.5 / static_cast<double>(height)},
        },
        .radius_level_zero_pixels = 1U,
        .mode = image::SpotRepairMode::clone,
        .source_offset_x_radii = 2.0,
        .source_offset_y_radii = 0.0,
        .feather = 0.0,
    };
    const std::array clone_nodes{
        image::AdjustmentNode{
            .node_id = "continuous-retouch-clone",
            .parameters = image::SpotHealAdjustment{.strokes = {clone_stroke}},
        },
    };
    const auto plan = image::compile_edit_execution_plan(clone_nodes);
    expect(
        plan.cumulative_footprint == image::AdjustmentFootprint{
            .horizontal_radius = 4U,
            .vertical_radius = 4U,
        },
        "continuous retouch strokes retain spot-heal detail-tile support"
    );
    const auto cloned = image::execute_adjustment_nodes(
        rgb_raster(width, height, clone_samples),
        clone_nodes
    );
    const std::size_t middle = (4U * width + 8U) * 3U;
    const std::size_t above_middle = (2U * width + 8U) * 3U;
    expect_close(
        cloned.samples[middle],
        0.5F,
        "clone stroke applies its fixed source offset through the continuous middle"
    );
    expect_close(
        cloned.samples[middle + 1U],
        0.2F,
        "clone stroke copies the corresponding source path vertically"
    );
    expect_close(
        cloned.samples[above_middle],
        0.4F,
        "continuous stroke leaves pixels outside its swept capsule unchanged"
    );

    std::vector<float> heal_samples(static_cast<std::size_t>(width) * height * 3U, 0.2F);
    for (std::uint32_t x = 4U; x <= 12U; ++x) {
        const std::size_t sample = (4U * width + x) * 3U;
        heal_samples[sample] = 1.0F;
        heal_samples[sample + 1U] = 0.0F;
        heal_samples[sample + 2U] = 0.0F;
    }
    const std::array heal_nodes{
        image::AdjustmentNode{
            .node_id = "continuous-retouch-heal",
            .parameters = image::SpotHealAdjustment{
                .strokes = {{
                    .points = clone_stroke.points,
                    .radius_level_zero_pixels = 1U,
                    .mode = image::SpotRepairMode::heal,
                    .feather = 0.0,
                }},
            },
        },
    };
    const auto healed = image::execute_adjustment_nodes(
        rgb_raster(width, height, heal_samples),
        heal_nodes
    );
    expect_close(
        healed.samples[middle],
        0.2F,
        "heal stroke reconstructs the complete swept region from its surrounding ring"
    );
    expect_close(
        healed.samples[middle + 1U],
        0.2F,
        "heal stroke removes the channel defect without exposing individual dabs"
    );

    const image::SpotHealAdjustment invalid{
        .strokes = {{.points = {}}},
    };
    expect_edit_error(
        [&] { image::validate_spot_heal(invalid); },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "continuous retouch strokes require at least one authored point"
    );
}

void photo_geometry_is_lossless_and_maps_detail_tiles_to_source_space() {
    const auto input = rgb_raster(
        3U,
        2U,
        {
            0.0F, 0.0F, 0.0F,
            1.0F, 1.0F, 1.0F,
            2.0F, 2.0F, 2.0F,
            3.0F, 3.0F, 3.0F,
            4.0F, 4.0F, 4.0F,
            5.0F, 5.0F, 5.0F,
        }
    );
    const image::PhotoGeometry clockwise{
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
    };
    const auto layout = image::photo_geometry_layout(input.dimensions, clockwise);
    expect(
        layout.output_dimensions == image::Dimensions{2U, 3U},
        "a quarter turn swaps the output canvas dimensions"
    );
    const auto output = image::apply_photo_geometry(input, clockwise);
    const std::array<float, 6U> expected_values{3.0F, 0.0F, 4.0F, 1.0F, 5.0F, 2.0F};
    for (std::size_t index = 0U; index < expected_values.size(); ++index) {
        expect_close(
            output.samples[index * 3U],
            expected_values[index],
            "quarter-turn geometry uses an exact source-pixel permutation"
        );
    }

    const image::GeometryPixelRect output_row{.x = 0U, .y = 1U, .width = 2U, .height = 1U};
    const auto required_source = image::photo_geometry_source_rect_for_output(
        layout,
        clockwise,
        output_row
    );
    expect(
        required_source == image::GeometryPixelRect{.x = 1U, .y = 0U, .width = 1U, .height = 2U},
        "a rotated output tile requests only its exact source-space rectangle"
    );
    const auto source_tile = rgb_raster(
        1U,
        2U,
        {1.0F, 1.0F, 1.0F, 4.0F, 4.0F, 4.0F}
    );
    const auto output_tile = image::apply_photo_geometry_tile(
        source_tile,
        required_source,
        layout,
        clockwise,
        output_row
    );
    expect_close(output_tile.samples[0], 4.0F, "geometry detail tile retains its first mapped pixel");
    expect_close(output_tile.samples[3], 1.0F, "geometry detail tile retains its second mapped pixel");

    // A minimally fetched detail tile on an integer source-pixel boundary has
    // no bilinear neighbour to provide. It must still render safely: the
    // zero-weight neighbour must not be dereferenced past the tile buffer.
    const image::PhotoGeometry identity_geometry{};
    const auto identity_layout = image::photo_geometry_layout(
        image::Dimensions{2U, 2U},
        identity_geometry
    );
    const auto boundary_tile = image::apply_photo_geometry_tile(
        rgb_raster(1U, 1U, {9.0F, 9.0F, 9.0F}),
        image::GeometryPixelRect{.x = 1U, .y = 1U, .width = 1U, .height = 1U},
        identity_layout,
        identity_geometry,
        image::GeometryPixelRect{.x = 1U, .y = 1U, .width = 1U, .height = 1U}
    );
    expect_close(
        boundary_tile.samples[0],
        9.0F,
        "integer-aligned minimal detail tiles never dereference a zero-weight neighbour"
    );

    const image::PhotoGeometry centered_crop{
        .crop_left = 1.0 / 3.0,
        .crop_top = 0.0,
        .crop_right = 1.0,
        .crop_bottom = 1.0,
    };
    const auto cropped = image::apply_photo_geometry(input, centered_crop);
    expect(
        cropped.dimensions == image::Dimensions{2U, 2U},
        "normalized crop edges resolve to a stable integer source rectangle"
    );
    expect_close(cropped.samples[0], 1.0F, "crop begins at the expected source column");
    expect_close(cropped.samples[9], 5.0F, "crop retains the expected final source pixel");

    std::vector<float> straighten_samples(5U * 5U * 3U, 0.0F);
    const std::size_t straighten_center = (2U * 5U + 2U) * 3U;
    straighten_samples[straighten_center] = 1.0F;
    straighten_samples[straighten_center + 1U] = 0.5F;
    straighten_samples[straighten_center + 2U] = 0.25F;
    const image::PhotoGeometry straighten{
        .straighten_degrees = 45.0,
    };
    const auto straightened = image::apply_photo_geometry(
        rgb_raster(5U, 5U, straighten_samples),
        straighten
    );
    expect(
        straightened.dimensions == image::Dimensions{3U, 3U},
        "fine straighten auto-crops a centered interior canvas"
    );
    expect_close(
        straightened.samples[(1U * 3U + 1U) * 3U],
        1.0F,
        "fine straighten keeps the exact rotation centre stable after auto-crop"
    );

    std::vector<float> filled_samples(64U * 48U * 3U, 0.8F);
    const image::PhotoGeometry auto_crop_straighten{
        .straighten_degrees = 15.0,
    };
    const auto auto_crop_layout = image::photo_geometry_layout(
        image::Dimensions{64U, 48U},
        auto_crop_straighten
    );
    expect(
        auto_crop_layout.output_dimensions.width < 64U
            && auto_crop_layout.output_dimensions.height < 48U,
        "fine straighten reduces both axes enough to remove empty corners"
    );
    const auto auto_cropped = image::apply_photo_geometry(
        rgb_raster(64U, 48U, filled_samples),
        auto_crop_straighten
    );
    for (const float sample : auto_cropped.samples) {
        expect_close(
            sample,
            0.8F,
            "fine straighten auto-crop never leaves an empty output corner"
        );
    }
}

} // namespace

int main() {
    global_effect_coordinates_are_tile_invariant();
    local_mask_layers_blend_complete_adjustments_in_global_coordinates();
    spot_heal_repairs_small_defects_in_global_coordinates();
    continuous_retouch_strokes_sweep_one_connected_repair_region();
    photo_geometry_is_lossless_and_maps_detail_tiles_to_source_space();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
