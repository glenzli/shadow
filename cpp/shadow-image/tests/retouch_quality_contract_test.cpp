#include "edit_contract_test_support.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/retouch.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

[[nodiscard]] std::size_t
sample_index(const std::uint32_t x, const std::uint32_t y, const std::uint32_t width) {
    return (static_cast<std::size_t>(y) * width + x) * 3U;
}

[[nodiscard]] std::array<float, 3U>
target_illumination(const std::uint32_t x, const std::uint32_t y) {
    return {
        0.18F + 0.006F * static_cast<float>(x) + 0.003F * static_cast<float>(y),
        0.16F + 0.004F * static_cast<float>(x) + 0.002F * static_cast<float>(y),
        0.20F + 0.003F * static_cast<float>(x) + 0.005F * static_cast<float>(y),
    };
}

void heal_tracks_local_illumination_instead_of_stamping_a_global_tone() {
    constexpr std::uint32_t width = 64U;
    constexpr std::uint32_t height = 48U;
    constexpr std::uint32_t target_x = 18U;
    constexpr std::uint32_t target_y = 24U;
    constexpr std::uint32_t donor_x = 43U;
    std::vector<float> samples(static_cast<std::size_t>(width) * height * 3U, 0.0F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto value = target_illumination(x, y);
            const std::size_t sample = sample_index(x, y, width);
            std::copy(
                value.begin(),
                value.end(),
                samples.begin() + static_cast<std::ptrdiff_t>(sample)
            );
        }
    }

    // The donor carries the opposite horizontal illumination slope. A global
    // mean shift would stamp that slope into the repair; an affine boundary
    // fit recovers the target's local light while retaining donor detail.
    for (std::uint32_t y = 16U; y <= 32U; ++y) {
        for (std::uint32_t x = donor_x - 8U; x <= donor_x + 8U; ++x) {
            const float local_x = static_cast<float>(static_cast<std::int32_t>(x) - 43);
            const float local_y = static_cast<float>(static_cast<std::int32_t>(y) - 24);
            const std::array value{
                0.72F - 0.008F * local_x + 0.001F * local_y,
                0.62F - 0.006F * local_x + 0.002F * local_y,
                0.68F - 0.004F * local_x - 0.001F * local_y,
            };
            const std::size_t sample = sample_index(x, y, width);
            std::copy(
                value.begin(),
                value.end(),
                samples.begin() + static_cast<std::ptrdiff_t>(sample)
            );
        }
    }
    for (std::uint32_t y = target_y - 2U; y <= target_y + 2U; ++y) {
        for (std::uint32_t x = target_x - 2U; x <= target_x + 2U; ++x) {
            const std::int32_t delta_x = static_cast<std::int32_t>(x) - 18;
            const std::int32_t delta_y = static_cast<std::int32_t>(y) - 24;
            if (delta_x * delta_x + delta_y * delta_y <= 4) {
                const std::size_t sample = sample_index(x, y, width);
                samples[sample] = 1.15F;
                samples[sample + 1U] = 0.05F;
                samples[sample + 2U] = 0.05F;
            }
        }
    }

    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "local-illumination-heal",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 18.5 / static_cast<double>(width),
                    .center_y = 24.5 / static_cast<double>(height),
                    .radius_level_zero_pixels = 5U,
                    .mode = image::SpotRepairMode::heal,
                    .source_offset_x_radii = 5.0,
                    .source_offset_y_radii = 0.0,
                    .feather = 0.0,
                }},
            },
        },
    };
    const auto healed = image::execute_adjustment_nodes(rgb_raster(width, height, samples), nodes);
    for (const std::uint32_t x : std::array{target_x - 3U, target_x, target_x + 3U}) {
        const auto expected = target_illumination(x, target_y);
        const std::size_t sample = sample_index(x, target_y, width);
        for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
            expect_close_double(
                healed.samples[sample + channel],
                expected[channel],
                0.025,
                "Heal follows the target's local affine illumination through the repaired core"
            );
        }
    }
    expect(
        healed.samples[sample_index(target_x + 3U, target_y, width)]
            > healed.samples[sample_index(target_x - 3U, target_y, width)],
        "Heal preserves the target light direction instead of the donor's opposite slope"
    );
}

void clone_preserves_high_frequency_source_structure_without_blur() {
    constexpr std::uint32_t width = 64U;
    constexpr std::uint32_t height = 32U;
    std::vector<float> samples(static_cast<std::size_t>(width) * height * 3U, 0.0F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const float checker = (x + y) % 2U == 0U ? 0.12F : 0.88F;
            const std::size_t sample = sample_index(x, y, width);
            samples[sample] = checker;
            samples[sample + 1U] = 1.0F - checker;
            samples[sample + 2U] = 0.2F + 0.01F * static_cast<float>(x);
        }
    }
    for (std::uint32_t y = 8U; y <= 12U; ++y) {
        for (std::uint32_t x = 14U; x <= 50U; ++x) {
            const std::size_t sample = sample_index(x, y, width);
            samples[sample] = 0.95F;
            samples[sample + 1U] = 0.05F;
            samples[sample + 2U] = 0.05F;
        }
    }
    const image::RetouchStroke stroke{
        .points =
            {
                {.x = 16.5 / static_cast<double>(width), .y = 10.5 / static_cast<double>(height)},
                {.x = 48.5 / static_cast<double>(width), .y = 10.5 / static_cast<double>(height)},
            },
        .radius_level_zero_pixels = 2U,
        .mode = image::SpotRepairMode::clone,
        .source_offset_x_radii = 0.0,
        .source_offset_y_radii = 4.0,
        .feather = 0.0,
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "high-frequency-clone",
            .parameters = image::SpotHealAdjustment{.strokes = {stroke}},
        },
    };
    const auto cloned = image::execute_adjustment_nodes(rgb_raster(width, height, samples), nodes);
    const std::size_t target = sample_index(32U, 10U, width);
    const std::size_t donor = sample_index(32U, 18U, width);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        expect_close(
            cloned.samples[target + channel],
            samples[donor + channel],
            "Clone preserves the selected source's high-frequency sample exactly"
        );
    }
    expect_close(
        cloned.samples[sample_index(32U, 6U, width)],
        samples[sample_index(32U, 6U, width)],
        "Clone leaves structure outside the swept brush unchanged"
    );
}

void one_point_clone_stroke_executes_as_a_click_authored_region() {
    constexpr std::uint32_t width = 40U;
    constexpr std::uint32_t height = 24U;
    constexpr std::uint32_t target_x = 12U;
    constexpr std::uint32_t target_y = 12U;
    constexpr std::uint32_t donor_x = 21U;
    constexpr std::uint16_t radius = 3U;
    std::vector<float> samples(static_cast<std::size_t>(width) * height * 3U, 0.0F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t sample = sample_index(x, y, width);
            samples[sample] = 0.01F * static_cast<float>(x);
            samples[sample + 1U] = 0.02F * static_cast<float>(y);
            samples[sample + 2U] = 0.01F * static_cast<float>(x + y);
        }
    }
    const image::RetouchStroke stroke{
        .points = {{
            .x = 12.5 / static_cast<double>(width),
            .y = 12.5 / static_cast<double>(height),
        }},
        .radius_level_zero_pixels = radius,
        .mode = image::SpotRepairMode::clone,
        .source_offset_x_radii = 3.0,
        .source_offset_y_radii = 0.0,
        .feather = 0.0,
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "one-point-click-clone",
            .parameters = image::SpotHealAdjustment{.strokes = {stroke}},
        },
    };
    const auto cloned = image::execute_adjustment_nodes(rgb_raster(width, height, samples), nodes);
    const std::size_t target = sample_index(target_x, target_y, width);
    const std::size_t donor = sample_index(donor_x, target_y, width);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        expect_close(
            cloned.samples[target + channel],
            samples[donor + channel],
            "a one-point stroke applies its clone donor at the click center"
        );
    }
    expect_close(
        cloned.samples[sample_index(2U, 2U, width)],
        samples[sample_index(2U, 2U, width)],
        "a one-point stroke leaves pixels outside its circular footprint unchanged"
    );
}

void clone_does_not_invent_a_cable_fitting_absent_from_the_donor() {
    constexpr std::uint32_t width = 160U;
    constexpr std::uint32_t height = 96U;
    constexpr std::uint32_t wire_y = 48U;
    constexpr std::uint32_t target_x = 110U;
    constexpr std::int32_t source_delta_x = -50;
    constexpr std::uint16_t radius = 6U;
    std::vector<float> samples(static_cast<std::size_t>(width) * height * 3U, 0.82F);
    for (std::uint32_t x = 0U; x < width; ++x) {
        const std::size_t wire = sample_index(x, wire_y, width);
        samples[wire] = 0.18F;
        samples[wire + 1U] = 0.18F;
        samples[wire + 2U] = 0.18F;
    }
    for (std::uint32_t y = wire_y - 3U; y <= wire_y + 3U; ++y) {
        const std::size_t fitting = sample_index(target_x, y, width);
        samples[fitting] = 0.02F;
        samples[fitting + 1U] = 0.02F;
        samples[fitting + 2U] = 0.02F;
    }

    const image::RetouchStroke stroke{
        .points =
            {
                {.x = 104.5 / static_cast<double>(width), .y = 48.5 / static_cast<double>(height)},
                {.x = 116.5 / static_cast<double>(width), .y = 48.5 / static_cast<double>(height)},
            },
        .radius_level_zero_pixels = radius,
        .mode = image::SpotRepairMode::clone,
        .source_offset_x_radii = static_cast<double>(source_delta_x) / radius,
        .source_offset_y_radii = 0.0,
        .feather = 0.28,
        .strength = 1.0,
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "clean-cable-clone",
            .parameters = image::SpotHealAdjustment{.strokes = {stroke}},
        },
    };
    const auto cloned = image::execute_adjustment_nodes(rgb_raster(width, height, samples), nodes);
    const std::size_t repaired = sample_index(target_x, wire_y, width);
    const std::size_t donor = sample_index(
        static_cast<std::uint32_t>(static_cast<std::int32_t>(target_x) + source_delta_x),
        wire_y,
        width
    );
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        expect_close(
            cloned.samples[repaired + channel],
            samples[donor + channel],
            "a full-strength clone removes the target fitting with the clean donor wire"
        );
    }

    for (std::uint32_t y = wire_y - radius; y <= wire_y + radius; ++y) {
        for (std::uint32_t x = target_x - 10U; x <= target_x + 10U; ++x) {
            const std::size_t target = sample_index(x, y, width);
            const std::size_t source = sample_index(
                static_cast<std::uint32_t>(static_cast<std::int32_t>(x) + source_delta_x),
                y,
                width
            );
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const float lower = std::min(samples[target + channel], samples[source + channel]);
                const float upper = std::max(samples[target + channel], samples[source + channel]);
                expect(
                    cloned.samples[target + channel] >= lower - 1.0e-6F
                        && cloned.samples[target + channel] <= upper + 1.0e-6F,
                    "clone output remains a convex blend of target and donor and cannot invent a "
                    "point"
                );
            }
        }
    }
}

void clone_applies_the_authored_source_transform_without_resampling_the_target_shape() {
    constexpr std::uint32_t width = 64U;
    constexpr std::uint32_t height = 48U;
    std::vector<float> samples(static_cast<std::size_t>(width) * height * 3U, 0.0F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t sample = sample_index(x, y, width);
            samples[sample] = static_cast<float>(x) / static_cast<float>(width);
            samples[sample + 1U] = static_cast<float>(y) / static_cast<float>(height);
            samples[sample + 2U] = static_cast<float>(x + y) / static_cast<float>(width + height);
        }
    }
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "transformed-clone-source",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 20.5 / static_cast<double>(width),
                    .center_y = 20.5 / static_cast<double>(height),
                    .radius_level_zero_pixels = 5U,
                    .mode = image::SpotRepairMode::clone,
                    .source_offset_x_radii = 2.0,
                    .source_offset_y_radii = 0.0,
                    .source_rotation_degrees = 90.0,
                    .source_scale = 1.5,
                    .source_flip_horizontal = true,
                    .feather = 0.0,
                }},
            },
        },
    };
    const auto cloned = image::execute_adjustment_nodes(rgb_raster(width, height, samples), nodes);
    const std::size_t target = sample_index(22U, 20U, width);
    const std::size_t transformed_source = sample_index(30U, 17U, width);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        expect_close_double(
            cloned.samples[target + channel],
            samples[transformed_source + channel],
            1.0e-5,
            "Clone maps scale, mirror, and rotation around the source anchor"
        );
    }
    const image::AdjustmentFootprint footprint = image::footprint(
        nodes[0].parameters,
        1.0,
        1.0,
        image::Dimensions{.width = width, .height = height}
    );
    expect(
        footprint.horizontal_radius < image::maximum_retouch_detail_apron_level_zero_pixels
            && footprint.vertical_radius < image::maximum_retouch_detail_apron_level_zero_pixels,
        "a bounded transformed source keeps an exact finite detail-tile footprint"
    );
}

void structure_heal_preserves_a_target_edge_that_crosses_the_repair() {
    constexpr std::uint32_t width = 64U;
    constexpr std::uint32_t height = 48U;
    constexpr std::uint32_t target_x = 18U;
    constexpr std::uint32_t target_y = 24U;
    std::vector<float> samples(static_cast<std::size_t>(width) * height * 3U, 0.58F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::size_t edge = sample_index(target_x, y, width);
        samples[edge] = 0.12F;
        samples[edge + 1U] = 0.12F;
        samples[edge + 2U] = 0.12F;
    }

    const auto repair = [](const image::SpotRepairMode mode) {
        return std::array{
            image::AdjustmentNode{
                .node_id = mode == image::SpotRepairMode::heal_structure
                               ? "structure-preserving-heal"
                               : "natural-heal",
                .parameters = image::SpotHealAdjustment{
                    .spots = {{
                        .center_x = 18.5 / static_cast<double>(width),
                        .center_y = 24.5 / static_cast<double>(height),
                        .radius_level_zero_pixels = 5U,
                        .mode = mode,
                        .source_offset_x_radii = 5.0,
                        .source_offset_y_radii = 0.0,
                        .feather = 0.0,
                    }},
                },
            },
        };
    };
    const auto natural = image::execute_adjustment_nodes(
        rgb_raster(width, height, samples),
        repair(image::SpotRepairMode::heal)
    );
    const auto structured = image::execute_adjustment_nodes(
        rgb_raster(width, height, samples),
        repair(image::SpotRepairMode::heal_structure)
    );
    const std::size_t edge = sample_index(target_x, target_y, width);
    const std::size_t neighbor = sample_index(target_x + 1U, target_y, width);
    const double natural_contrast =
        static_cast<double>(natural.samples[neighbor]) - natural.samples[edge];
    const double structured_contrast =
        static_cast<double>(structured.samples[neighbor]) - structured.samples[edge];
    expect(
        structured_contrast > natural_contrast + 0.12,
        "Structure Heal retains a target edge that natural Heal correctly treats as a defect"
    );
    expect(
        structured.samples[edge] < 0.42F,
        "Structure Heal keeps the crossing line visibly darker than its donor"
    );
}

void frequency_retouch_preserves_the_other_component_and_tile_result() {
    constexpr std::uint32_t width = 256, height = 160;
    auto input = image::FloatRgbImage{};
    input.dimensions = {width, height};
    input.row_stride_bytes = width * 3 * sizeof(float);
    input.level_zero_to_raster_scale_x = 1;
    input.level_zero_to_raster_scale_y = 1;
    input.samples.resize(width * height * 3);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                input.samples[(y * width + x) * 3 + c] =
                    -0.2F + float(c) * 1.3F + float(x) * 0.001F + (x % 2 ? 0.05F : -0.05F);
    image::SpotHealAdjustment repair{
        .spots = {{
            .center_x = 128.5 / width,
            .center_y = 80.5 / height,
            .radius_level_zero_pixels = 12,
            .mode = image::SpotRepairMode::tone,
            .source_offset_x_radii = 1.75,
            .feather = 0.0,
            .frequency_radius = 8,
        }}
    };
    auto tone = input, texture = input, clone = input;
    image::apply_spot_heal(tone, repair);
    repair.spots[0].mode = image::SpotRepairMode::texture;
    image::apply_spot_heal(texture, repair);
    repair.spots[0].mode = image::SpotRepairMode::clone;
    image::apply_spot_heal(clone, repair);
    for (std::size_t i = 0; i < input.samples.size(); ++i)
        expect(
            std::abs(
                double(tone.samples[i]) + texture.samples[i] - clone.samples[i] - input.samples[i]
            ) < 0.000002,
            "tone and texture edits must recompose the complete clone without clipping signed/HDR "
            "samples"
        );
    const auto center = (80 * width + 128) * 3;
    expect(
        std::abs(tone.samples[center] - input.samples[center] - 0.021F) < 0.0002,
        "tone-only repair must retain alternating fine texture"
    );
    expect(
        std::abs(texture.samples[center] - input.samples[center] - 0.1F) < 0.0002,
        "texture-only repair must retain the local illumination slope"
    );
    auto tile = input;
    tile.dimensions = {128, 112};
    tile.row_stride_bytes = 128 * 3 * sizeof(float);
    tile.samples.resize(128 * 112 * 3);
    for (std::uint32_t y = 0; y < 112; ++y)
        for (std::uint32_t x = 0; x < 128; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                tile.samples[(y * 128 + x) * 3 + c] =
                    input.samples[((y + 24) * width + x + 64) * 3 + c];
    repair.spots[0].mode = image::SpotRepairMode::texture;
    image::apply_spot_heal(
        tile,
        repair,
        {.origin_x = 64, .origin_y = 24, .full_dimensions = input.dimensions}
    );
    double maximum_error = 0;
    for (std::uint32_t y = 68; y <= 92; ++y)
        for (std::uint32_t x = 116; x <= 140; ++x)
            for (std::uint32_t c = 0; c < 3; ++c) {
                const double actual = tile.samples[((y - 24) * 128 + x - 64) * 3 + c];
                const double expected = texture.samples[(y * width + x) * 3 + c];
                maximum_error = std::max(maximum_error, std::abs(actual - expected));
            }
    expect(
        maximum_error < 0.000001,
        "frequency repair must agree across full-image and bounded detail-tile execution"
    );
    auto untouched = input;
    repair.spots[0].strength = 0;
    image::apply_spot_heal(untouched, repair);
    expect(
        untouched.samples == input.samples,
        "zero-strength frequency repair must be exact identity"
    );
    repair.spots[0].strength = 0.8;
    repair.spots[0].mode = image::SpotRepairMode::tone;
    repair.spots[0].source_offset_x_radii = 2.9;
    repair.spots[0].frequency_radius = 6;
    repair.spots.push_back(
        {.center_x = 144.5 / width,
         .center_y = 80.5 / height,
         .radius_level_zero_pixels = 12,
         .mode = image::SpotRepairMode::texture,
         .source_offset_x_radii = -1.7,
         .feather = 0.3,
         .frequency_radius = 8}
    );
    auto complete = input;
    image::apply_spot_heal(complete, repair);
    const auto reach = image::footprint(repair, 1, 1, input.dimensions);
    const auto left = std::max(0, 139 - int(reach.horizontal_radius));
    const auto top = std::max(0, 77 - int(reach.vertical_radius));
    const auto right = std::min(int(width), 147 + int(reach.horizontal_radius));
    const auto bottom = std::min(int(height), 84 + int(reach.vertical_radius));
    auto bounded = input;
    bounded.dimensions = {std::uint32_t(right - left), std::uint32_t(bottom - top)};
    bounded.row_stride_bytes = bounded.dimensions.width * 3 * sizeof(float);
    bounded.samples.resize(bounded.dimensions.width * bounded.dimensions.height * 3);
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x)
            for (std::size_t c = 0; c < 3; ++c)
                bounded.samples
                    [(std::size_t(y - top) * bounded.dimensions.width + std::size_t(x - left)) * 3
                     + c] = input.samples[(std::size_t(y) * width + std::size_t(x)) * 3 + c];
    image::apply_spot_heal(
        bounded,
        repair,
        {.origin_x = std::uint32_t(left),
         .origin_y = std::uint32_t(top),
         .full_dimensions = input.dimensions}
    );
    double chained_error = 0;
    for (int y = 77; y < 84; ++y)
        for (int x = 139; x < 147; ++x)
            for (std::size_t c = 0; c < 3; ++c)
                chained_error = std::max(
                    chained_error,
                    std::abs(
                        double(bounded.samples
                                   [(std::size_t(y - top) * bounded.dimensions.width
                                     + std::size_t(x - left))
                                        * 3
                                    + c])
                        - complete.samples[(std::size_t(y) * width + std::size_t(x)) * 3 + c]
                    )
                );
    expect(
        chained_error < 0.000002,
        "dependency-limited tiles must preserve overlapping ordered tone/texture repairs"
    );
}

void nearby_frequency_regions_do_not_pay_unrelated_source_displacements_twice() {
    image::SpotHealAdjustment repair{
        .spots = {
            {.center_x = 400.5 / 1024,
             .center_y = 128.5 / 256,
             .radius_level_zero_pixels = 64,
             .mode = image::SpotRepairMode::tone,
             .source_offset_x_radii = 6.0,
             .frequency_radius = 12},
            {.center_x = 475.5 / 1024,
             .center_y = 128.5 / 256,
             .radius_level_zero_pixels = 64,
             .mode = image::SpotRepairMode::texture,
             .source_offset_x_radii = -0.85,
             .frequency_radius = 21}
        }
    };
    const auto reach = image::footprint(repair, 1.0, 1.0, {1024, 256});
    expect(
        reach.horizontal_radius <= 512 && reach.vertical_radius <= 512,
        "nearby tone and texture repair must fit the bounded detail footprint"
    );
    // The first donor is still required: reducing the bound must not forget it.
    expect(
        reach.horizontal_radius >= 420,
        "frequency footprint retains its far donor and blur halo"
    );
}

} // namespace

int main() {
    nearby_frequency_regions_do_not_pay_unrelated_source_displacements_twice();
    frequency_retouch_preserves_the_other_component_and_tile_result();
    heal_tracks_local_illumination_instead_of_stamping_a_global_tone();
    clone_preserves_high_frequency_source_structure_without_blur();
    one_point_clone_stroke_executes_as_a_click_authored_region();
    clone_does_not_invent_a_cable_fitting_absent_from_the_donor();
    clone_applies_the_authored_source_transform_without_resampling_the_target_shape();
    structure_heal_preserves_a_target_edge_that_crosses_the_repair();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
