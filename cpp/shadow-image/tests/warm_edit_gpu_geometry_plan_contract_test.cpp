#include "../src/proxy/warm_edit_gpu_geometry_plan.hpp"
#include "../src/edit/photo_liquify_sampling.hpp"

#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/photo_structural_rendering.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void complete_canvas_and_bounded_tiles_share_one_mapping_contract() {
    constexpr image::Dimensions dimensions{320U, 200U};
    const image::PhotoGeometry geometry{
        .crop_left = 0.1,
        .crop_top = 0.15,
        .crop_right = 0.9,
        .crop_bottom = 0.85,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 7.0,
        .flip_horizontal = true,
    };
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const image::GeometryPixelRect complete_output{
        .width = layout.output_dimensions.width,
        .height = layout.output_dimensions.height,
    };
    const image::GeometryPixelRect complete_source{
        .width = dimensions.width,
        .height = dimensions.height,
    };
    const auto complete = image::detail::prepare_warm_gpu_geometry_plan(
        dimensions,
        dimensions.width * 3U,
        0.5,
        0.25,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = complete_source,
            .output_rect = complete_output,
        }
    );
    expect(
        complete.plan.has_value() && complete.plan->valid()
            && complete.plan->output_dimensions == layout.output_dimensions
            && complete.plan->parameters.output_canvas_width == layout.output_dimensions.width
            && complete.plan->parameters.quarter_turn == 1U
            && complete.plan->output_level_zero_to_raster_scale_x == 0.25
            && complete.plan->output_level_zero_to_raster_scale_y == 0.5,
        "complete transposed geometry lowers the authoritative canvas and native scales"
    );

    const image::GeometryPixelRect output_tile{
        .x = layout.output_dimensions.width / 4U,
        .y = layout.output_dimensions.height / 5U,
        .width = layout.output_dimensions.width / 3U,
        .height = layout.output_dimensions.height / 2U,
    };
    const auto source_tile =
        image::photo_geometry_source_rect_for_output(layout, geometry, output_tile);
    const auto bounded = image::detail::prepare_warm_gpu_geometry_plan(
        {source_tile.width, source_tile.height},
        source_tile.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = source_tile,
            .output_rect = output_tile,
        }
    );
    expect(
        bounded.plan.has_value() && bounded.plan->valid()
            && bounded.plan->parameters.source_tile_origin_x == source_tile.x
            && bounded.plan->parameters.source_tile_origin_y == source_tile.y
            && bounded.plan->parameters.output_origin_x == output_tile.x
            && bounded.plan->parameters.output_origin_y == output_tile.y,
        "bounded detail tiles retain full-canvas output and source-space origins"
    );

    auto incomplete_source = source_tile;
    --incomplete_source.width;
    const auto incomplete = image::detail::prepare_warm_gpu_geometry_plan(
        {incomplete_source.width, incomplete_source.height},
        incomplete_source.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = incomplete_source,
            .output_rect = output_tile,
        }
    );
    expect(
        !incomplete.plan.has_value() && !incomplete.diagnostic.empty(),
        "a source tile missing any bilinear footprint declines before Metal encoding"
    );

    auto displaced_source = source_tile;
    displaced_source.x += displaced_source.width + 1U;
    const auto displaced = image::detail::prepare_warm_gpu_geometry_plan(
        {displaced_source.width, displaced_source.height},
        displaced_source.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = displaced_source,
            .output_rect = output_tile,
        }
    );
    expect(
        !displaced.plan.has_value() && !displaced.diagnostic.empty(),
        "a disjoint source tile cannot pass containment through unsigned subtraction"
    );
}

void bounded_liquify_index_preserves_reverse_candidates() {
    constexpr image::Dimensions dimensions{320U, 200U};
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.1,
        .crop_right = 0.92,
        .crop_bottom = 0.9,
        .straighten_degrees = 3.0,
    };
    const image::PhotoLiquify liquify{
        .strokes = {
            image::PhotoLiquifyPushStroke{
                .points = {
                    {.x = 0.18, .y = 0.35, .pressure = 0.7},
                    {.x = 0.42, .y = 0.42, .pressure = 1.0},
                    {.x = 0.62, .y = 0.37, .pressure = 0.55},
                },
                .radius = 0.12,
                .strength = 0.65,
                .hardness = 0.35,
            },
            image::PhotoLiquifyPushStroke{
                .points = {
                    {.x = 0.68, .y = 0.7, .pressure = 0.8},
                    {.x = 0.52, .y = 0.55, .pressure = 1.0},
                },
                .radius = 0.09,
                .strength = 0.45,
                .hardness = 0.7,
            },
        },
    };
    const auto prepared = image::prepare_photo_liquify(dimensions, liquify);
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const image::GeometryPixelRect output{
        .width = layout.output_dimensions.width,
        .height = layout.output_dimensions.height,
    };
    const image::GeometryPixelRect source{
        .width = dimensions.width,
        .height = dimensions.height,
    };
    const auto lowered = image::detail::prepare_warm_gpu_geometry_plan(
        dimensions,
        dimensions.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = source,
            .output_rect = output,
            .liquify = &prepared,
        }
    );
    expect(
        lowered.plan.has_value() && lowered.plan->valid()
            && lowered.plan->liquify_parameters.stamp_count == prepared.stamps.size()
            && lowered.plan->liquify_parameters.reference_count > prepared.stamps.size()
            && !lowered.plan->liquify_words.empty(),
        "prepared Liquify lowers into one bounded immutable Metal side table"
    );
    if (!lowered.plan.has_value()) {
        return;
    }

    const auto geometry_only =
        image::photo_geometry_source_rect_for_output(layout, geometry, output);
    const auto structural = image::photo_structural_source_rect_for_output(
        layout,
        geometry,
        &prepared,
        output
    );
    expect(
        structural.x <= geometry_only.x && structural.y <= geometry_only.y
            && structural.width >= geometry_only.width
            && structural.height >= geometry_only.height,
        "structural source admission expands the Canvas preimage for Liquify"
    );

    const auto& plan = *lowered.plan;
    const auto& parameters = plan.liquify_parameters;
    for (std::uint32_t y = 0U; y < dimensions.height; y += 11U) {
        for (std::uint32_t x = 0U; x < dimensions.width; x += 13U) {
            const std::uint32_t column = std::min(
                static_cast<std::uint32_t>(
                    static_cast<float>(x) / parameters.cell_width
                ),
                parameters.grid_columns - 1U
            );
            const std::uint32_t row = std::min(
                static_cast<std::uint32_t>(
                    static_cast<float>(y) / parameters.cell_height
                ),
                parameters.grid_rows - 1U
            );
            const std::uint32_t cell = row * parameters.grid_columns + column;
            const std::size_t range = static_cast<std::size_t>(
                parameters.cell_range_offset_words + cell * 2U
            );
            const std::uint32_t offset = plan.liquify_words[range].value;
            const std::uint32_t count = plan.liquify_words[range + 1U].value;

            double indexed_x = x;
            double indexed_y = y;
            std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t candidate = 0U; candidate < count; ++candidate) {
                const std::uint32_t stamp_index =
                    plan.liquify_words[
                        parameters.reference_offset_words + offset + candidate
                    ]
                        .value;
                expect(
                    stamp_index < previous,
                    "each Liquify cell retains reverse authoring order"
                );
                previous = stamp_index;
                const auto& stamp = prepared.stamps[stamp_index];
                const double weight = image::detail::photo_liquify_stamp_weight(
                    stamp,
                    indexed_x,
                    indexed_y
                );
                indexed_x -= stamp.displacement_x * weight;
                indexed_y -= stamp.displacement_y * weight;
            }
            const auto complete = image::detail::inverse_photo_liquify_coordinate(
                prepared,
                x,
                y
            );
            expect(
                std::abs(indexed_x - complete.x) <= 1.0e-12
                    && std::abs(indexed_y - complete.y) <= 1.0e-12,
                "bounded Liquify candidates preserve the complete inverse replay"
            );
        }
    }

    struct AbiStamp final {
        float center_x = 0.0F;
        float center_y = 0.0F;
        float displacement_x = 0.0F;
        float displacement_y = 0.0F;
        float radius = 0.0F;
        float hardness = 0.0F;
    };
    std::vector<AbiStamp> abi_stamps;
    abi_stamps.reserve(parameters.stamp_count);
    const auto abi_float = [&](const std::size_t index) {
        return std::bit_cast<float>(plan.liquify_words[index].value);
    };
    for (std::uint32_t stamp = 0U; stamp < parameters.stamp_count; ++stamp) {
        const std::size_t word = static_cast<std::size_t>(stamp) * 6U;
        abi_stamps.push_back(
            AbiStamp{
                .center_x = abi_float(word),
                .center_y = abi_float(word + 1U),
                .displacement_x = abi_float(word + 2U),
                .displacement_y = abi_float(word + 3U),
                .radius = abi_float(word + 4U),
                .hardness = abi_float(word + 5U),
            }
        );
    }
    const auto apply_abi_stamp = [](const AbiStamp& stamp, float& x, float& y) {
        const float distance = std::hypot(x - stamp.center_x, y - stamp.center_y);
        if (distance >= stamp.radius) {
            return;
        }
        const float normalized_distance = distance / stamp.radius;
        float weight = 1.0F;
        if (stamp.hardness < 1.0F && normalized_distance > stamp.hardness) {
            const float position =
                (normalized_distance - stamp.hardness) / (1.0F - stamp.hardness);
            const float bounded = std::clamp(position, 0.0F, 1.0F);
            const float smoother = bounded * bounded * bounded
                                   * (bounded * (bounded * 6.0F - 15.0F) + 10.0F);
            weight = std::clamp(1.0F - smoother, 0.0F, 1.0F);
        }
        x -= stamp.displacement_x * weight;
        y -= stamp.displacement_y * weight;
    };
    const auto indexed_abi_replay = [&](float x, float y) {
        const float bounded_x =
            std::clamp(x, 0.0F, static_cast<float>(parameters.source_width - 1U));
        const float bounded_y =
            std::clamp(y, 0.0F, static_cast<float>(parameters.source_height - 1U));
        const std::uint32_t column = std::min(
            static_cast<std::uint32_t>(bounded_x / parameters.cell_width),
            parameters.grid_columns - 1U
        );
        const std::uint32_t row = std::min(
            static_cast<std::uint32_t>(bounded_y / parameters.cell_height),
            parameters.grid_rows - 1U
        );
        const std::size_t range = static_cast<std::size_t>(
            parameters.cell_range_offset_words
            + (row * parameters.grid_columns + column) * 2U
        );
        const std::uint32_t offset = plan.liquify_words[range].value;
        const std::uint32_t count = plan.liquify_words[range + 1U].value;
        for (std::uint32_t candidate = 0U; candidate < count; ++candidate) {
            const std::uint32_t stamp = plan.liquify_words[
                parameters.reference_offset_words + offset + candidate
            ]
                                            .value;
            apply_abi_stamp(abi_stamps[stamp], x, y);
        }
        return std::pair{x, y};
    };
    const auto complete_abi_replay = [&](float x, float y) {
        for (auto stamp = abi_stamps.rbegin(); stamp != abi_stamps.rend(); ++stamp) {
            apply_abi_stamp(*stamp, x, y);
        }
        return std::pair{x, y};
    };
    std::vector<float> boundary_x{0.0F, static_cast<float>(dimensions.width - 1U)};
    for (std::uint32_t column = 1U; column < parameters.grid_columns; ++column) {
        const float boundary = parameters.cell_width * static_cast<float>(column);
        boundary_x.push_back(std::nextafter(boundary, 0.0F));
        boundary_x.push_back(boundary);
        boundary_x.push_back(
            std::nextafter(boundary, std::numeric_limits<float>::infinity())
        );
    }
    std::vector<float> boundary_y{0.0F, static_cast<float>(dimensions.height - 1U)};
    for (std::uint32_t row = 1U; row < parameters.grid_rows; ++row) {
        const float boundary = parameters.cell_height * static_cast<float>(row);
        boundary_y.push_back(std::nextafter(boundary, 0.0F));
        boundary_y.push_back(boundary);
        boundary_y.push_back(
            std::nextafter(boundary, std::numeric_limits<float>::infinity())
        );
    }
    for (const float y : boundary_y) {
        for (const float x : boundary_x) {
            const auto indexed = indexed_abi_replay(x, y);
            const auto complete = complete_abi_replay(x, y);
            expect(
                std::abs(indexed.first - complete.first) <= 1.0e-5F
                    && std::abs(indexed.second - complete.second) <= 1.0e-5F,
                "float-ABI cell boundaries retain every reachable Liquify stamp"
            );
        }
    }

    auto hard_edged = prepared;
    for (auto& stamp : hard_edged.stamps) {
        stamp.hardness = 1.0;
    }
    const auto hard_edged_lowering = image::detail::prepare_warm_gpu_geometry_plan(
        dimensions,
        dimensions.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = source,
            .output_rect = output,
            .liquify = &hard_edged,
        }
    );
    expect(
        !hard_edged_lowering.plan.has_value()
            && hard_edged_lowering.diagnostic.find("hard-edged") != std::string::npos,
        "discontinuous hard-edge stamps decline to the CPU structural sampler"
    );

    const image::PreparedPhotoLiquify invalid_empty{};
    const auto invalid_lowering = image::detail::prepare_warm_gpu_geometry_plan(
        dimensions,
        dimensions.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = source,
            .output_rect = output,
            .liquify = &invalid_empty,
        }
    );
    expect(
        !invalid_lowering.plan.has_value()
            && invalid_lowering.diagnostic.find("invalid prepared stamp plan")
                != std::string::npos,
        "node absence is null; a non-null empty prepared Liquify remains invalid"
    );
}

void metal_float_bound_expands_detail_tile_admission() {
    constexpr image::Dimensions dimensions{320U, 200U};
    const image::PhotoGeometry geometry{};
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const image::PreparedPhotoLiquify prepared{
        .source_dimensions = dimensions,
        .stamps = {
            image::PreparedPhotoLiquifyStamp{
                .center_x = 80.0,
                .center_y = 70.0,
                .displacement_x = 0.6,
                .displacement_y = 0.8,
                .radius = 20.0,
                .hardness = 0.5,
            },
        },
        .maximum_displacement_pixels = 1.0,
    };
    expect(prepared.valid(), "the tile-admission fixture is a valid CPU Liquify plan");
    const image::GeometryPixelRect output{
        .x = 70U,
        .y = 60U,
        .width = 24U,
        .height = 20U,
    };
    const auto cpu_source = image::photo_structural_source_rect_for_output(
        layout,
        geometry,
        &prepared,
        output
    );
    const auto cpu_sized = image::detail::prepare_warm_gpu_geometry_plan(
        {cpu_source.width, cpu_source.height},
        cpu_source.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = cpu_source,
            .output_rect = output,
            .liquify = &prepared,
        }
    );
    expect(
        !cpu_sized.plan.has_value() && !cpu_sized.diagnostic.empty(),
        "Metal rejects a detail tile that covers only the narrower double-CPU bound"
    );

    const image::GeometryPixelRect padded_source{
        .x = cpu_source.x - 1U,
        .y = cpu_source.y - 1U,
        .width = cpu_source.width + 2U,
        .height = cpu_source.height + 2U,
    };
    const auto padded = image::detail::prepare_warm_gpu_geometry_plan(
        {padded_source.width, padded_source.height},
        padded_source.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = padded_source,
            .output_rect = output,
            .liquify = &prepared,
        }
    );
    expect(
        padded.plan.has_value() && padded.plan->valid(),
        "one additional pixel admits the quantized Metal displacement bound"
    );
}

} // namespace

int main() {
    complete_canvas_and_bounded_tiles_share_one_mapping_contract();
    bounded_liquify_index_preserves_reverse_candidates();
    metal_float_bound_expands_detail_tile_admission();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
