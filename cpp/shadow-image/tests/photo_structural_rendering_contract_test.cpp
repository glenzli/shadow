#include "edit_contract_test_support.hpp"

#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/photo_structural_rendering.hpp>
#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

[[nodiscard]] image::FloatRgbImage gradient(const std::uint32_t side = 9U) {
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(side) * side * 3U);
    for (std::uint32_t y = 0U; y < side; ++y) {
        for (std::uint32_t x = 0U; x < side; ++x) {
            samples.push_back(static_cast<float>(x) / static_cast<float>(side - 1U));
            samples.push_back(static_cast<float>(y) / static_cast<float>(side - 1U));
            samples.push_back(
                static_cast<float>(x + y) / static_cast<float>((side - 1U) * 2U)
            );
        }
    }
    return rgb_raster(side, side, std::move(samples));
}

[[nodiscard]] image::PhotoLiquify horizontal_push() {
    return image::PhotoLiquify{
        .strokes =
            {
                image::PhotoLiquifyPushStroke{
                    .points =
                        {
                            {.x = 0.3, .y = 0.5, .pressure = 1.0},
                            {.x = 0.7, .y = 0.5, .pressure = 1.0},
                        },
                    .radius = 0.25,
                    .strength = 0.75,
                    .hardness = 0.5,
                },
            },
    };
}

[[nodiscard]] image::FloatRgbImage crop_source_tile(
    const image::FloatRgbImage& source,
    const image::GeometryPixelRect rect
) {
    image::FloatRgbImage tile = source;
    tile.dimensions = {rect.width, rect.height};
    tile.row_stride_bytes = static_cast<std::size_t>(rect.width) * 3U * sizeof(float);
    tile.samples.clear();
    tile.samples.reserve(static_cast<std::size_t>(rect.width) * rect.height * 3U);
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        const std::size_t offset =
            (static_cast<std::size_t>(rect.y + y) * source.dimensions.width + rect.x) * 3U;
        tile.samples.insert(
            tile.samples.end(),
            source.samples.begin() + static_cast<std::ptrdiff_t>(offset),
            source.samples.begin()
                + static_cast<std::ptrdiff_t>(offset + rect.width * 3U)
        );
    }
    return tile;
}

void geometry_only_plan_matches_the_existing_canvas_sampler() {
    const auto source = gradient();
    const image::PhotoGeometry geometry{
        .crop_left = 0.1,
        .crop_top = 0.2,
        .crop_right = 0.9,
        .crop_bottom = 0.8,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .flip_horizontal = true,
    };
    const auto structural =
        image::prepare_photo_structural_rendering(source.dimensions, geometry);
    const auto expected = image::apply_photo_geometry(source, geometry);
    const auto actual = image::apply_photo_structural_rendering(source, structural);
    expect(actual.dimensions == expected.dimensions, "structural Canvas keeps output dimensions");
    expect(actual.samples == expected.samples, "geometry-only structural sampling is exact parity");
}

void identity_tiles_preserve_exact_hdr_samples_metadata_and_apron_offsets() {
    auto source = gradient(11U);
    for (auto& sample : source.samples) {
        sample = sample * 7.0F - 2.0F;
    }
    source.working_space = linear_rec2020();
    source.level_zero_to_raster_scale_x = 0.5;
    source.level_zero_to_raster_scale_y = 0.25;
    const auto structural = image::prepare_photo_structural_rendering(source.dimensions, {});
    const image::GeometryPixelRect source_rect{2U, 1U, 9U, 10U};
    const auto tile_source = crop_source_tile(source, source_rect);
    for (const auto output_rect : {
             source_rect,
             image::GeometryPixelRect{4U, 3U, 4U, 5U},
             image::GeometryPixelRect{10U, 10U, 1U, 1U},
         }) {
        const auto expected = crop_source_tile(source, output_rect);
        const auto actual = image::apply_photo_structural_rendering_tile(
            tile_source, source_rect, structural, output_rect
        );
        expect(actual.samples == expected.samples, "identity tile copies exact HDR RGB samples");
        expect(actual.dimensions == expected.dimensions, "identity tile keeps requested extent");
        expect(actual.row_stride_bytes == expected.row_stride_bytes, "identity tile is tightly packed");
        expect(actual.working_space == expected.working_space, "identity tile retains working space");
        expect(actual.reference == expected.reference, "identity tile retains image reference");
        expect(actual.transfer_function == expected.transfer_function, "identity tile retains transfer");
        expect(actual.level_zero_to_raster_scale_x == 0.5
                   && actual.level_zero_to_raster_scale_y == 0.25,
               "identity tile retains anisotropic sampling density");
    }
    bool rejected = false;
    try {
        static_cast<void>(image::apply_photo_structural_rendering_tile(
            tile_source, source_rect, structural, image::GeometryPixelRect{1U, 1U, 2U, 2U}
        ));
    } catch (const image::DecodeError&) {
        rejected = true;
    }
    expect(rejected, "identity copy still rejects missing source preimage");
}

void identity_canvas_fuses_to_the_same_single_liquify_sample() {
    const auto source = gradient();
    const auto liquify = horizontal_push();
    const auto prepared_liquify =
        image::prepare_photo_liquify(source.dimensions, liquify);
    const auto expected = image::apply_photo_liquify(source, prepared_liquify);
    const auto structural = image::prepare_photo_structural_rendering(
        source.dimensions,
        image::PhotoGeometry{},
        &liquify
    );
    const auto actual = image::apply_photo_structural_rendering(source, structural);
    expect(actual.samples == expected.samples, "identity Canvas adds no second interpolation");
}

void conservative_preimage_makes_detail_tiles_match_the_full_fused_render() {
    const auto source = gradient(17U);
    const auto liquify = horizontal_push();
    const image::PhotoGeometry geometry{
        .crop_left = 0.15,
        .crop_top = 0.1,
        .crop_right = 0.9,
        .crop_bottom = 0.85,
    };
    const auto structural =
        image::prepare_photo_structural_rendering(source.dimensions, geometry, &liquify);
    const auto full = image::apply_photo_structural_rendering(source, structural);
    const image::GeometryPixelRect output_rect{
        .x = 2U,
        .y = 3U,
        .width = std::min(5U, full.dimensions.width - 2U),
        .height = std::min(4U, full.dimensions.height - 3U),
    };
    const auto source_rect =
        image::photo_structural_source_rect_for_output(structural, output_rect);
    const auto geometry_only = image::photo_geometry_source_rect_for_output(
        structural.geometry_layout,
        structural.geometry,
        output_rect
    );
    expect(
        source_rect.x <= geometry_only.x && source_rect.y <= geometry_only.y
            && source_rect.width >= geometry_only.width
            && source_rect.height >= geometry_only.height,
        "Liquify conservatively expands the Canvas source preimage"
    );
    const auto tile_source = crop_source_tile(source, source_rect);
    const auto tile = image::apply_photo_structural_rendering_tile(
        tile_source,
        source_rect,
        structural,
        output_rect
    );
    for (std::uint32_t y = 0U; y < output_rect.height; ++y) {
        for (std::uint32_t x = 0U; x < output_rect.width; ++x) {
            const std::size_t tile_index =
                (static_cast<std::size_t>(y) * output_rect.width + x) * 3U;
            const std::size_t full_index =
                (
                    static_cast<std::size_t>(output_rect.y + y) * full.dimensions.width
                    + output_rect.x + x
                ) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                expect_close(
                    tile.samples[tile_index + channel],
                    full.samples[full_index + channel],
                    "detail tile equals the corresponding full fused output"
                );
            }
        }
    }
}

} // namespace

int main() {
    identity_tiles_preserve_exact_hdr_samples_metadata_and_apron_offsets();
    geometry_only_plan_matches_the_existing_canvas_sampler();
    identity_canvas_fuses_to_the_same_single_liquify_sample();
    conservative_preimage_makes_detail_tiles_match_the_full_fused_render();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
