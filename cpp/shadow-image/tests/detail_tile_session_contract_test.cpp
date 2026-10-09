#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/warm_edit_preview.hpp>

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

[[nodiscard]] image::DetailTileRenderBackend available_reference_gpu_backend() {
    // Probe a separate public consumer: the staged adjustment dispatcher reports only Metal,
    // while resident preview/detail sessions may select CUDA.
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto preview = image::prepare_warm_edit_preview(decoder, dimensions.width);
    if (!preview.gpu_stats().resident) {
        return image::DetailTileRenderBackend::cpu;
    }
    const auto rendered = preview.render_jpeg_with_analysis(neutral_plan());
    expect(rendered.execution.valid(), "independent GPU probe carries a valid receipt");
    return rendered.execution.display_backend == image::EditPreviewBackend::cuda
        ? image::DetailTileRenderBackend::cuda
        : image::DetailTileRenderBackend::metal;
}

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
    const auto gpu_backend = available_reference_gpu_backend();
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
    if (gpu_backend != image::DetailTileRenderBackend::cpu) {
        expect(
            first.execution.backend == gpu_backend
                && !first.execution.source_cache_hit
                && !first.execution.fell_back && first.execution.diagnostic.empty()
                && second.execution.backend == gpu_backend
                && second.execution.source_cache_hit
                && !second.execution.fell_back && second.execution.diagnostic.empty(),
            "a repeated detail viewport reuses its resident GPU source upload"
        );
    } else {
        expect(
            first.execution.backend == image::DetailTileRenderBackend::cpu
                && first.execution.fell_back && !first.execution.diagnostic.empty(),
            "automatic detail rendering records an unavailable-GPU CPU fallback"
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

void automatic_layer_tiles_record_backend_and_fallback_without_redecoding() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto nodes = neutral_plan();
    const std::array layers{image::AdjustmentLayer{
        .layer_id = "neutral-layer",
        .nodes = {nodes.begin(), nodes.end()},
    }};
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
    const auto gpu_backend = available_reference_gpu_backend();
    const auto rendered = session.render_rgb8_layers(layers, {1, 1, 2, 2});
    expect(rendered.execution.valid(), "automatic layer rendering has a consistent receipt");
    expect(decoder.render_count() == 1, "automatic layer fallback retains the prepared source");
    if (gpu_backend != image::DetailTileRenderBackend::cpu) {
        expect(
            rendered.execution.backend == gpu_backend
                && !rendered.execution.fell_back && rendered.execution.diagnostic.empty(),
            "available GPU layers report direct execution"
        );
    } else {
        expect(
            rendered.execution.backend == image::DetailTileRenderBackend::cpu
                && rendered.execution.fell_back && !rendered.execution.diagnostic.empty(),
            "automatic layer CPU fallback keeps its diagnostic paired with the fallback flag"
        );
    }
    if (gpu_backend == image::DetailTileRenderBackend::cuda) {
        auto masked_layers = layers;
        masked_layers[0].nodes[0].parameters = image::ExposureAdjustment{.stops = 0.5};
        masked_layers[0].mask = image::LocalMask{
            .kind = image::LocalMaskKind::linear_gradient,
            .x0 = 0.0,
            .y0 = 0.0,
            .x1 = 1.0,
            .y1 = 1.0,
        };
        const auto fallback = session.render_rgb8_layers(masked_layers, {1, 1, 2, 2});
        expect(
            fallback.execution.valid()
                && fallback.execution.backend == image::DetailTileRenderBackend::cpu
                && fallback.execution.fell_back && !fallback.execution.diagnostic.empty(),
            "unsupported CUDA masks retain a consistent automatic CPU fallback receipt"
        );
        const ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
        const auto reference = session.render_rgb8_layers(masked_layers, {1, 1, 2, 2});
        expect(fallback.bytes == reference.bytes, "CUDA rejection replays the exact CPU layer plan");
        expect(decoder.render_count() == 1, "CUDA rejection reuses the prepared source");
    }
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
    automatic_layer_tiles_record_backend_and_fallback_without_redecoding();
    adjustments_apply_only_to_the_requested_crop();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
