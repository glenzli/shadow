#include "detail_tile_contract_test_support.hpp"

#include <shadow/image/full_edit_detail.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::neutral_plan;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::SyntheticDecodeSession;

void irregular_tiles_match_one_full_pixel_local_execution_without_seams() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.5},
        },
        image::AdjustmentNode{
            .node_id = "contrast",
            .parameters = image::ContrastAdjustment{.factor = 1.2, .pivot = 0.18},
        },
        image::AdjustmentNode{
            .node_id = "curve",
            .parameters =
                image::OklabLightnessToneCurve{
                    .lightness =
                        image::ToneCurveSet{
                            .points = {{0.0, 0.05}, {0.45, 0.3}, {1.0, 0.95}},
                        },
                },
        },
        image::AdjustmentNode{
            .node_id = "white-balance",
            .parameters =
                image::RgbWhiteBalanceAdjustment{
                    .temperature = 0.1,
                    .tint = -0.05,
                },
        },
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.8},
        },
        image::AdjustmentNode{
            .node_id = "disabled",
            .enabled = false,
            .parameters = image::ExposureAdjustment{.stops = 4.0},
        },
    };
    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    for (const image::DetailTileRect rect : std::array{
             image::DetailTileRect{0, 0, 1, 3},
             image::DetailTileRect{1, 0, 3, 1},
             image::DetailTileRect{1, 1, 2, 2},
             image::DetailTileRect{3, 1, 1, 2},
         }) {
        const auto tile = session.render_rgb8(plan, rect);
        for (std::uint32_t row = 0; row < rect.height; ++row) {
            const auto source =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                source,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "irregular tiles match one full execution across nonlinear and disabled nodes"
    );
}

void neighborhood_tiles_accumulate_two_sharpen_footprints_without_seams() {
    constexpr image::Dimensions dimensions{160, 50};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "wide-sharpen-first",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .amount = 0.7,
                    .radius = 5.0,
                    .threshold = 0.05,
                    .masking = 0.2,
                },
        },
        image::AdjustmentNode{
            .node_id = "wide-sharpen-second",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 0.4,
                .radius = 5.0,
                .threshold = 0.1,
                .masking = 0.5,
            },
        },
    };
    const auto first_support = image::footprint(plan[0].parameters);
    const auto second_support = image::footprint(plan[1].parameters);
    expect(
        first_support.horizontal_radius + second_support.horizontal_radius == 30U,
        "two radius-five sharpen nodes require the sum of both 15-pixel supports"
    );

    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array x_segments{
        std::pair<std::uint32_t, std::uint32_t>{0U, 37U},
        std::pair<std::uint32_t, std::uint32_t>{37U, 41U},
        std::pair<std::uint32_t, std::uint32_t>{78U, 82U},
    };
    constexpr std::array y_segments{
        std::pair<std::uint32_t, std::uint32_t>{0U, 19U},
        std::pair<std::uint32_t, std::uint32_t>{19U, 31U},
    };
    for (const auto [x, width] : x_segments) {
        for (const auto [y, height] : y_segments) {
            const image::DetailTileRect rect{x, y, width, height};
            const auto tile = session.render_rgb8(plan, rect);
            for (std::uint32_t row = 0U; row < height; ++row) {
                const auto source =
                    tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
                const std::size_t destination =
                    (static_cast<std::size_t>(y + row) * dimensions.width + x) * 3U;
                std::copy_n(
                    source,
                    static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                    stitched.begin() + static_cast<std::ptrdiff_t>(destination)
                );
            }
        }
    }
    expect(
        stitched == full.bytes,
        "two sequential neighborhood nodes render identically as full and irregular tiled images"
    );
}

void guided_selective_tone_tiles_match_full_execution_at_edges_and_boundaries() {
    constexpr image::Dimensions dimensions{224, 72};
    auto source = reference_rgb(dimensions);
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            // A broad dark region with gentle texture meets a three-stop highlight edge. This
            // exercises both the self-guided mask and a tile boundary that crosses the edge.
            const double base = x < 112U
                                    ? 0.045 + 0.004 * std::sin(static_cast<double>(x + y) * 0.18)
                                    : 0.72 + 0.02 * std::cos(static_cast<double>(y) * 0.24);
            const auto encoded = static_cast<std::uint16_t>(
                std::clamp(std::llround(base * 65'535.0), 0LL, 65'535LL)
            );
            const std::size_t offset = (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            source.samples[offset] = encoded;
            source.samples[offset + 1U] = encoded;
            source.samples[offset + 2U] = encoded;
        }
    }
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(source));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "guided-selective-tone",
            .parameter_schema_version = image::selective_tone_parameter_schema_version,
            .implementation_version = image::selective_tone_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .highlights = -0.55,
                .shadows = 0.7,
                .whites = -0.15,
                .blacks = 0.2,
            },
        },
    };
    expect(
        image::footprint(plan[0].parameters).horizontal_radius
            == static_cast<std::uint32_t>(
                image::selective_tone_guided_mask_radius_level_zero
                * image::selective_tone_guided_filter_box_passes
            ),
        "complete guided selective tone declares both box-pass supports for its detail-tile apron"
    );
    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array tiles{
        image::DetailTileRect{0U, 0U, 73U, 29U},
        image::DetailTileRect{73U, 0U, 76U, 29U},
        image::DetailTileRect{149U, 0U, 75U, 29U},
        image::DetailTileRect{0U, 29U, 73U, 43U},
        image::DetailTileRect{73U, 29U, 76U, 43U},
        image::DetailTileRect{149U, 29U, 75U, 43U},
    };
    for (const image::DetailTileRect rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect);
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "guided selective tone produces identical full-frame and apron-expanded tiled cores"
    );
}

void composed_neighborhood_tiles_match_one_resident_execution_without_seams() {
    constexpr image::Dimensions dimensions{224, 72};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "guided-selective-tone",
            .parameter_schema_version = image::selective_tone_parameter_schema_version,
            .implementation_version = image::selective_tone_implementation_version,
            .parameters =
                image::SelectiveToneAdjustment{
                    .highlights = -0.42,
                    .shadows = 0.56,
                    .whites = -0.18,
                    .blacks = 0.22,
                },
        },
        image::AdjustmentNode{
            .node_id = "between-stage-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.18},
        },
        image::AdjustmentNode{
            .node_id = "technical-sharpen",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::technical_detail,
                    .amount = 0.28,
                    .radius = 1.2,
                    .threshold = 0.06,
                    .masking = 0.25,
                },
        },
        image::AdjustmentNode{
            .node_id = "full-resolution-texture-clarity",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                .clarity = 0.31,
                .texture = 0.24,
            },
        },
    };
    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array tiles{
        image::DetailTileRect{0U, 0U, 73U, 29U},
        image::DetailTileRect{73U, 0U, 76U, 29U},
        image::DetailTileRect{149U, 0U, 75U, 29U},
        image::DetailTileRect{0U, 29U, 73U, 43U},
        image::DetailTileRect{73U, 29U, 76U, 43U},
        image::DetailTileRect{149U, 29U, 75U, 43U},
    };
    for (const image::DetailTileRect rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect);
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "composed selective tone, sharpen and full-resolution Clarity preserve tiled seams"
    );
}

void displaced_heal_tiles_include_the_donor_and_match_full_execution() {
    constexpr image::Dimensions dimensions{96, 64};
    auto source = reference_rgb(dimensions);
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(source));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "displaced-texture-heal",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 40.5 / static_cast<double>(dimensions.width),
                    .center_y = 32.5 / static_cast<double>(dimensions.height),
                    .radius_level_zero_pixels = 4U,
                    .mode = image::SpotRepairMode::heal,
                    .source_offset_x_radii = 4.0,
                    .source_offset_y_radii = 0.0,
                    .feather = 0.25,
                }},
            },
        },
    };
    expect(
        image::footprint(plan[0].parameters).horizontal_radius == 21U,
        "displaced Heal declares its donor, brush radius and "
        "gradient-neighbor apron"
    );

    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array tiles{
        image::DetailTileRect{0U, 0U, 41U, 29U},
        image::DetailTileRect{41U, 0U, 55U, 29U},
        image::DetailTileRect{0U, 29U, 41U, 35U},
        image::DetailTileRect{41U, 29U, 55U, 35U},
    };
    for (const image::DetailTileRect rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect);
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "gradient-domain Heal produces identical "
        "full-frame and donor-apron tile output"
    );
}

} // namespace

int main() {
    irregular_tiles_match_one_full_pixel_local_execution_without_seams();
    neighborhood_tiles_accumulate_two_sharpen_footprints_without_seams();
    guided_selective_tone_tiles_match_full_execution_at_edges_and_boundaries();
    composed_neighborhood_tiles_match_one_resident_execution_without_seams();
    displaced_heal_tiles_include_the_donor_and_match_full_execution();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
