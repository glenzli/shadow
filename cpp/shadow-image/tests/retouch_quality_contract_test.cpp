#include "edit_contract_test_support.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

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

} // namespace

int main() {
    heal_tracks_local_illumination_instead_of_stamping_a_global_tone();
    clone_preserves_high_frequency_source_structure_without_blur();
    clone_applies_the_authored_source_transform_without_resampling_the_target_shape();
    structure_heal_preserves_a_target_edge_that_crosses_the_repair();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
