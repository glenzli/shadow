#include "contract_test_assertions.hpp"
#include "processed_rgb_session_fixture.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/edit_error.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <array>
#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::processed_linear_gradient;
using shadow::image::test_support::RetainedRgbSession;
using shadow::image::test_support::ScopedEnvironment;

[[nodiscard]] std::array<image::AdjustmentLayer, 2U> supported_layers() {
    return {
        image::AdjustmentLayer{
            .layer_id = "linear",
            .opacity = 0.72,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::linear_gradient,
                    .x0 = 0.15,
                    .y0 = 0.2,
                    .x1 = 0.8,
                    .y1 = 0.7,
                },
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "linear-exposure",
                        .parameters = image::ExposureAdjustment{.stops = 0.35},
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "radial",
            .opacity = 0.83,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::radial_gradient,
                    .x0 = 0.55,
                    .y0 = 0.48,
                    .radius_x = 0.28,
                    .radius_y = 0.35,
                    .feather = 0.6,
                },
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "radial-saturation",
                    .parameters = image::SaturationAdjustment{.factor = 1.18},
                },
            },
        },
    };
}

void supported_layers_publish_the_effective_resident_route() {
    const RetainedRgbSession session(processed_linear_gradient());
    const auto warm = image::prepare_warm_edit_preview(session, 96U);
    const auto layers = supported_layers();

    image::AnalyzedEditPreview cpu;
    {
        const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
        cpu = warm.render_jpeg_with_analysis_layers(layers, 90U);
    }
    expect(
        cpu.execution.valid() && cpu.execution.adjustment_backend == image::EditPreviewBackend::cpu
            && cpu.execution.display_backend == image::EditPreviewBackend::cpu
            && !cpu.execution.adjustment_fell_back && !cpu.execution.display_fell_back,
        "forced CPU layer rendering retains the staged CPU receipt"
    );

    if (warm.gpu_stats().resident) {
        image::AnalyzedEditPreview metal;
        {
            const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
            metal = warm.render_jpeg_with_analysis_layers(layers, 90U);
        }
        expect(
            metal.execution.valid()
                && metal.execution.adjustment_backend == image::EditPreviewBackend::metal
                && metal.execution.display_backend == image::EditPreviewBackend::metal
                && metal.execution.fused_pipeline && !metal.execution.adjustment_fell_back
                && !metal.execution.display_fell_back && metal.execution.diagnostic.empty(),
            "supported local-mask layers publish one fused resident Metal receipt"
        );
    } else {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "resident layer Metal was required but unavailable"
        );
    }
}

void brush_layers_use_resident_metal_and_failure_replays_atomically() {
    const RetainedRgbSession session(processed_linear_gradient());
    const auto warm = image::prepare_warm_edit_preview(session, 96U);
    const std::array brush_layers{
        image::AdjustmentLayer{
            .layer_id = "brush",
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::brush,
                    .radius_x = 0.08,
                    .feather = 0.5,
                    .points =
                        {
                            {.x = 0.2, .y = 0.3, .begins_stroke = true},
                            {.x = 0.7, .y = 0.6},
                        },
                },
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "brush-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.4},
                },
            },
        },
    };
    if (warm.gpu_stats().resident) {
        {
            const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
            const auto metal = warm.render_jpeg_with_analysis_layers(brush_layers, 90U);
            expect(
                metal.execution.valid()
                    && metal.execution.adjustment_backend == image::EditPreviewBackend::metal
                    && metal.execution.display_backend == image::EditPreviewBackend::metal
                    && metal.execution.fused_pipeline && !metal.execution.adjustment_fell_back
                    && !metal.execution.display_fell_back,
                "continuous brush layers publish one fused resident Metal receipt"
            );
        }
        const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
        const ScopedEnvironment injected_failure("SHADOW_TEST_WARM_METAL_FORCE_FAILURE", "1");
        const auto fallback = warm.render_jpeg_with_analysis_layers(brush_layers, 90U);
        expect(
            fallback.execution.valid()
                && fallback.execution.adjustment_backend == image::EditPreviewBackend::cpu
                && fallback.execution.display_backend == image::EditPreviewBackend::cpu
                && fallback.execution.adjustment_fell_back && fallback.execution.display_fell_back
                && !fallback.execution.diagnostic.empty(),
            "a failed indexed brush transaction replays adjustment and display atomically on CPU"
        );
        try {
            const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
            static_cast<void>(warm.render_jpeg_layers(brush_layers, 90U));
            expect(false, "forced Metal must reject an injected brush transaction failure");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "forced brush Metal failure preserves typed backend semantics"
            );
        }
    } else {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "resident indexed brush Metal was required but unavailable"
        );
    }
}

} // namespace

int main() {
    supported_layers_publish_the_effective_resident_route();
    brush_layers_use_resident_metal_and_failure_replays_atomically();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
