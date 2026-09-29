#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/full_edit_detail.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::neutral_plan;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void preparation_retains_one_immutable_source_and_tiles_exactly() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto source_bytes =
        static_cast<std::uint64_t>(dimensions.pixel_count()) * 3U * sizeof(std::uint16_t);
    const auto session = image::prepare_full_edit_detail(decoder);
    expect(decoder.render_count() == 1, "detail preparation renders the source exactly once");
    expect(session.dimensions() == dimensions, "detail session retains full dimensions");
    expect(
        session.retained_bytes() >= source_bytes
            && session.retained_bytes() <= image::maximum_full_edit_detail_retained_bytes,
        "detail session reports a bounded allocation covering every u16 sample"
    );

    const image::DetailTileRect rect{1, 1, 2, 2};
    const auto plan = neutral_plan();
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
    const auto first = session.render_rgb8(plan, rect);
    const auto second = session.render_rgb8(plan, rect);
    expect(decoder.render_count() == 1, "tile renders never ask the decoder to render again");
    expect(first.rect == rect, "tile result preserves its exact rectangle");
    expect(first.full_dimensions == dimensions, "tile result carries full dimensions");
    expect(first.row_stride_bytes == 6U, "RGB8 tile rows are tightly packed");
    expect(first.bytes == second.bytes, "identical detail renders are deterministic");
    expect(
        first.execution.valid() && second.execution.valid(),
        "detail renders carry valid execution receipts"
    );
    if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        expect(
            first.execution.backend == image::DetailTileRenderBackend::metal
                && !first.execution.source_cache_hit
                && second.execution.backend == image::DetailTileRenderBackend::metal
                && second.execution.source_cache_hit,
            "a repeated detail viewport reuses its resident Metal source upload"
        );
    } else {
        expect(
            first.execution.backend == image::DetailTileRenderBackend::cpu
                && first.execution.fell_back && !first.execution.diagnostic.empty(),
            "automatic detail rendering records an unavailable-Metal CPU fallback"
        );
    }
    expect(
        first.bytes[0] > first.bytes[2] && first.bytes[1] > first.bytes[2]
            && first.bytes[4] > first.bytes[3] && first.bytes[5] > first.bytes[3]
            && first.bytes[6] > first.bytes[7] && first.bytes[8] > first.bytes[7]
            && first.bytes[9] == 0U && first.bytes[10] == 0U && first.bytes[11] == 0U,
        "detail crop preserves full-resolution pixel coordinates and primary-color dominance"
    );
}

void adjustments_apply_only_to_the_requested_crop() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "black-lift",
            .parameters = image::OklabLightnessToneCurve{
                .lightness = image::ToneCurveSet{
                    .points = {{0.0, 0.25}, {1.0, 1.0}},
                },
            },
        },
    };
    const auto adjusted = session.render_rgb8(plan, {0, 0, 1, 1});
    expect(
        adjusted.bytes[0] > 0U && adjusted.bytes[0] == adjusted.bytes[1]
            && adjusted.bytes[1] == adjusted.bytes[2],
        "detail tile executes scene-linear nodes before the neutral display transform"
    );
    const auto neutral = session.render_rgb8(neutral_plan(), {0, 0, 1, 1});
    expect(
        neutral.bytes == std::vector<std::uint8_t>{0, 0, 0},
        "render-local edits never mutate the retained source"
    );
}

void rgb_curves_match_full_detail_and_preserve_source() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    image::RgbToneCurves curves;
    curves.channels[0].points = {{0, 0.01}, {0.5, 0.55}, {1, 1}};
    curves.channels[1].points = {{0, 0.05}, {0.5, 0.6}, {1, 1}};
    curves.channels[3].points = {{0, 0}, {0.5, 0.45}, {1, 1}};
    const std::array plan{image::AdjustmentNode{.node_id = "rgb-curves", .parameters = curves}};
    const auto full = session.render_rgb8(plan, {0, 0, 4, 3});
    const auto tile = session.render_rgb8(plan, {0, 0, 1, 1});
    expect(
        tile.bytes == std::vector<std::uint8_t>(full.bytes.begin(), full.bytes.begin() + 3),
        "RGB curves match full-detail and cropped detail pixels"
    );
    expect(tile.bytes[0] > tile.bytes[1], "red channel curve reaches the detail renderer");
    expect(
        session.render_rgb8(neutral_plan(), {0, 0, 1, 1}).bytes
            == std::vector<std::uint8_t>{0, 0, 0},
        "RGB curve detail leaves retained source immutable"
    );
}

} // namespace

int main() {
    rgb_curves_match_full_detail_and_preserve_source();
    preparation_retains_one_immutable_source_and_tiles_exactly();
    adjustments_apply_only_to_the_requested_crop();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
