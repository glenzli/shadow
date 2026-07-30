#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void public_preview_liquify_uses_metal_when_resident_and_cpu_otherwise() {
    constexpr image::Dimensions dimensions{160U, 112U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto warm = image::prepare_warm_edit_preview(decoder, dimensions.width);
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.1,
        .crop_right = 0.93,
        .crop_bottom = 0.9,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 2.5,
    };
    const image::PhotoLiquify liquify{
        .strokes =
            {
                image::PhotoLiquifyPushStroke{
                    .points =
                        {
                            {.x = 0.33, .y = 0.48, .pressure = 1.0},
                            {.x = 0.58, .y = 0.52, .pressure = 0.7},
                        },
                    .radius = 0.16,
                    .strength = 0.4,
                    .hardness = 0.6,
                },
            },
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "liquify-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.21},
        },
    };
    image::EncodedProxy cpu;
    {
        const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
        cpu = warm.render_rgb8(nodes, geometry, &liquify);
    }
    image::EncodedProxy automatic;
    image::AnalyzedEditPreview analyzed;
    {
        const ScopedEnvironment auto_backend("SHADOW_IMAGE_ACCELERATION", "auto");
        automatic = warm.render_rgb8(nodes, geometry, &liquify);
        analyzed = warm.render_jpeg_with_analysis(nodes, 90U, geometry, &liquify);
    }
    std::uint8_t maximum_difference = 0U;
    if (automatic.bytes.size() == cpu.bytes.size()) {
        for (std::size_t index = 0U; index < cpu.bytes.size(); ++index) {
            const auto difference = static_cast<std::uint8_t>(
                std::max(automatic.bytes[index], cpu.bytes[index])
                - std::min(automatic.bytes[index], cpu.bytes[index])
            );
            maximum_difference = std::max(maximum_difference, difference);
        }
    }
    expect(
        automatic.dimensions == cpu.dimensions && automatic.bytes.size() == cpu.bytes.size()
            && maximum_difference <= 1U,
        "automatic Liquify remains within one RGB8 level of the CPU structural oracle"
    );
    if (warm.gpu_stats().resident) {
        expect(
            analyzed.execution.valid()
                && analyzed.execution.adjustment_backend == image::EditPreviewBackend::metal
                && analyzed.execution.display_backend == image::EditPreviewBackend::metal
                && analyzed.execution.fused_pipeline
                && !analyzed.execution.adjustment_fell_back
                && !analyzed.execution.display_fell_back
                && analyzed.execution.diagnostic.empty(),
            "resident Liquify preview reports one fused Metal structural route"
        );
    } else {
        expect(
            analyzed.execution.valid()
                && analyzed.execution.adjustment_backend == image::EditPreviewBackend::cpu
                && analyzed.execution.display_backend == image::EditPreviewBackend::cpu
                && analyzed.execution.adjustment_fell_back
                && analyzed.execution.display_fell_back
                && !analyzed.execution.diagnostic.empty(),
            "unavailable Metal replays the complete Liquify request through CPU"
        );
    }
}

void liquify_keeps_local_mask_coverage_paired_with_the_preview() {
    constexpr image::Dimensions dimensions{160U, 112U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto warm = image::prepare_warm_edit_preview(decoder, dimensions.width);
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.1,
        .crop_right = 0.93,
        .crop_bottom = 0.9,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 2.5,
    };
    const image::PhotoLiquify liquify{
        .strokes = {
            image::PhotoLiquifyPushStroke{
                .points = {
                    {.x = 0.28, .y = 0.46, .pressure = 0.35},
                    {.x = 0.62, .y = 0.54, .pressure = 1.0},
                },
                .radius = 0.16,
                .strength = 0.4,
                .hardness = 0.6,
            },
        },
    };
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "liquify-coverage",
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::radial_gradient,
                    .x0 = 0.52,
                    .y0 = 0.48,
                    .radius_x = 0.31,
                    .radius_y = 0.24,
                    .feather = 0.55,
                },
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "liquify-coverage-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.18},
                },
            },
        },
    };
    image::CancellableEditPreviewResult<image::EditPreviewRgb8WithMaskCoverage> cpu;
    image::CancellableEditPreviewResult<image::EditPreviewRgb8WithMaskCoverage> canvas_only;
    {
        const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
        cpu = warm.render_rgb8_layers_with_mask_coverage_cancellable(
            layers,
            0U,
            {},
            geometry,
            &liquify
        );
        canvas_only = warm.render_rgb8_layers_with_mask_coverage_cancellable(
            layers,
            0U,
            {},
            geometry
        );
    }
    image::CancellableEditPreviewResult<image::EditPreviewRgb8WithMaskCoverage> automatic;
    {
        const ScopedEnvironment auto_backend("SHADOW_IMAGE_ACCELERATION", "auto");
        automatic = warm.render_rgb8_layers_with_mask_coverage_cancellable(
            layers,
            0U,
            {},
            geometry,
            &liquify
        );
    }
    expect(
        cpu.completed.has_value() && automatic.completed.has_value()
            && canvas_only.completed.has_value()
            && cpu.completed->mask_coverage.has_value()
            && automatic.completed->mask_coverage.has_value()
            && canvas_only.completed->mask_coverage.has_value(),
        "Liquify does not disable paired local-mask coverage"
    );
    if (!cpu.completed || !automatic.completed || !cpu.completed->mask_coverage
        || !automatic.completed->mask_coverage || !canvas_only.completed
        || !canvas_only.completed->mask_coverage) {
        return;
    }
    const auto& cpu_coverage = *cpu.completed->mask_coverage;
    const auto& automatic_coverage = *automatic.completed->mask_coverage;
    std::uint8_t maximum_difference = 0U;
    if (cpu_coverage.samples.size() == automatic_coverage.samples.size()) {
        for (std::size_t index = 0U; index < cpu_coverage.samples.size(); ++index) {
            maximum_difference = std::max(
                maximum_difference,
                static_cast<std::uint8_t>(
                    std::max(cpu_coverage.samples[index], automatic_coverage.samples[index])
                    - std::min(cpu_coverage.samples[index], automatic_coverage.samples[index])
                )
            );
        }
    }
    expect(
        cpu_coverage.valid() && automatic_coverage.valid()
            && cpu_coverage.dimensions == cpu.completed->preview.dimensions
            && automatic_coverage.dimensions == automatic.completed->preview.dimensions
            && cpu_coverage.dimensions == automatic_coverage.dimensions
            && cpu_coverage.samples.size() == automatic_coverage.samples.size()
            && maximum_difference <= 1U,
        "Liquify transforms paired R8 coverage through the same CPU-or-Metal structural route"
    );
    expect(
        cpu_coverage.samples != canvas_only.completed->mask_coverage->samples,
        "paired coverage visibly includes Liquify rather than Canvas alone"
    );
}

} // namespace

int main() {
    public_preview_liquify_uses_metal_when_resident_and_cpu_otherwise();
    liquify_keeps_local_mask_coverage_paired_with_the_preview();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
