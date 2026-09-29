#include "contract_test_assertions.hpp"
#include "processed_rgb_session_fixture.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stop_token>
#include <vector>

namespace image = shadow::image;

namespace {

using image::test_support::expect;
using image::test_support::failures;
using image::test_support::processed_linear_gradient;
using image::test_support::RetainedRgbSession;
using image::test_support::ScopedEnvironment;

[[nodiscard]] image::AdjustmentNode exposure_node() {
    return image::AdjustmentNode{
        .node_id = "coverage-exposure",
        .parameters = image::ExposureAdjustment{.stops = 0.6},
    };
}

[[nodiscard]] image::AdjustmentNode no_op_node() {
    return image::AdjustmentNode{
        .node_id = "coverage-no-op",
        .parameters = image::ExposureAdjustment{.stops = 0.0},
    };
}

[[nodiscard]] image::LocalMask constant_managed_mask(const std::uint8_t coverage) {
    return image::LocalMask{
        .kind = image::LocalMaskKind::managed_raster,
        .managed_raster = image::ManagedRasterMask{
            .raster_dimensions = {.width = 1U, .height = 1U},
            .coordinate_dimensions = {.width = 6'000U, .height = 4'000U},
            .samples = {coverage},
        },
    };
}

[[nodiscard]] image::LocalMask composite_mask(
    const std::array<image::LocalMaskComponentOperation, 4U>& operations,
    const std::array<std::uint8_t, 4U>& coverages,
    const bool invert = false
) {
    image::LocalMask mask{.invert = invert};
    for (std::size_t index = 0U; index < operations.size(); ++index) {
        mask.components.push_back(
            image::LocalMaskComponent{
                .operation = operations[index],
                .mask = constant_managed_mask(coverages[index]),
            }
        );
    }
    return mask;
}

[[nodiscard]] std::array<image::LocalMask, 6U> representative_masks() {
    return {
        image::LocalMask{
            .kind = image::LocalMaskKind::linear_gradient,
            .x0 = 0.12,
            .y0 = 0.18,
            .x1 = 0.84,
            .y1 = 0.77,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::radial_gradient,
            .x0 = 0.53,
            .y0 = 0.46,
            .radius_x = 0.33,
            .radius_y = 0.28,
            .feather = 0.55,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::brush,
            .radius_x = 0.09,
            .feather = 0.48,
            .points =
                {
                    {.x = 0.16, .y = 0.24, .begins_stroke = true},
                    {.x = 0.72, .y = 0.63},
                },
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::luminance_range,
            .x0 = 0.24,
            .x1 = 0.72,
            .feather = 0.10,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::color_range,
            .x0 = 330.0 / 360.0,
            .x1 = 54.0 / 180.0,
            .feather = 0.38,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::condition_expression,
            .condition_program = {
                {0.0, 0.24, 0.76, 0.1, 0.0, 0.0, 0.0, 0.0},
                {1.0, 342.0, 60.0, 0.4, 0.08, 0.15, 0.0, 0.0},
                {2.0, 0.0, 0.06, 0.08, 0.0, 0.0, 0.0, 0.0},
                {6.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
                {5.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
                {4.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
            },
        },
    };
}

void supported_kinds_publish_geometrically_paired_r8() {
    const RetainedRgbSession source(processed_linear_gradient(97U, 61U));
    const auto warm = image::prepare_warm_edit_preview(source, 97U);
    const ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.11,
        .crop_right = 0.91,
        .crop_bottom = 0.88,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 3.0,
        .flip_horizontal = true,
    };
    for (const auto& mask : representative_masks()) {
        const std::array layers{
            image::AdjustmentLayer{
                .layer_id = "coverage-target",
                .mask = mask,
                .nodes = {exposure_node()},
            },
        };
        const auto paired =
            warm.render_rgb8_layers_with_mask_coverage_cancellable(layers, 0U, {}, geometry);
        expect(
            paired.completed.has_value() && paired.completed->mask_coverage.has_value(),
            "each local-mask kind publishes paired coverage"
        );
        if (!paired.completed || !paired.completed->mask_coverage) {
            continue;
        }
        const auto& coverage = *paired.completed->mask_coverage;
        expect(
            coverage.valid() && coverage.dimensions == paired.completed->preview.dimensions
                && coverage.row_stride_bytes == coverage.dimensions.width,
            "coverage is valid tightly packed R8 with preview geometry"
        );
        const auto [minimum, maximum] =
            std::minmax_element(coverage.samples.begin(), coverage.samples.end());
        expect(
            minimum != coverage.samples.end() && *minimum < *maximum,
            "representative coverage contains a useful feathered selection"
        );
    }
}

void target_semantics_capture_without_mutating_the_frame() {
    const RetainedRgbSession source(processed_linear_gradient(80U, 48U));
    const auto warm = image::prepare_warm_edit_preview(source, 80U);
    const ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    const std::array baseline_layers{
        image::AdjustmentLayer{
            .layer_id = "baseline-no-op",
            .nodes = {no_op_node()},
        },
    };
    const auto baseline = warm.render_rgb8_layers(baseline_layers);
    const image::LocalMask mask{
        .kind = image::LocalMaskKind::linear_gradient,
        .x0 = 0.1,
        .y0 = 0.2,
        .x1 = 0.9,
        .y1 = 0.7,
    };
    const std::array cases{
        image::AdjustmentLayer{
            .layer_id = "disabled",
            .enabled = false,
            .mask = mask,
            .nodes = {exposure_node()},
        },
        image::AdjustmentLayer{
            .layer_id = "zero-opacity",
            .opacity = 0.0,
            .mask = mask,
            .nodes = {exposure_node()},
        },
        image::AdjustmentLayer{
            .layer_id = "no-op",
            .mask = mask,
            .nodes = {no_op_node()},
        },
    };
    for (const auto& layer : cases) {
        const std::array layers{layer};
        const auto paired = warm.render_rgb8_layers_with_mask_coverage_cancellable(layers, 0U, {});
        expect(
            paired.completed.has_value() && paired.completed->mask_coverage.has_value()
                && paired.completed->preview.bytes == baseline.bytes,
            "disabled, zero-opacity and no-op targets capture without changing RGB"
        );
    }

    const std::array no_mask{
        image::AdjustmentLayer{
            .layer_id = "unmasked",
            .nodes = {no_op_node()},
        },
    };
    const auto absent = warm.render_rgb8_layers_with_mask_coverage_cancellable(no_mask, 0U, {});
    expect(
        absent.completed.has_value() && !absent.completed->mask_coverage.has_value(),
        "a valid target without a mask publishes no coverage"
    );
    const auto no_target =
        warm.render_rgb8_layers_with_mask_coverage_cancellable(no_mask, std::nullopt, {});
    expect(
        no_target.completed.has_value() && !no_target.completed->mask_coverage.has_value(),
        "an omitted target preserves the ordinary preview contract"
    );
}

void analyzed_jpeg_keeps_the_same_coverage_transaction() {
    const RetainedRgbSession source(processed_linear_gradient(88U, 52U));
    const auto warm = image::prepare_warm_edit_preview(source, 88U);
    const ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "analyzed-coverage",
            .mask = representative_masks()[1U],
            .nodes = {exposure_node()},
        },
    };
    const auto result =
        warm.render_jpeg_with_analysis_layers_and_mask_coverage_cancellable(layers, 0U, 91U, {});
    expect(
        result.completed.has_value() && result.completed->mask_coverage.has_value()
            && result.completed->mask_coverage->valid()
            && result.completed->preview.analysis.sample_dimensions
                   == result.completed->preview.proxy.dimensions
            && result.completed->mask_coverage->dimensions
                   == result.completed->preview.proxy.dimensions,
        "JPEG, analysis and mask coverage publish one geometrically paired transaction"
    );
}

void composite_coverage_is_idempotent_ordered_and_component_selectable() {
    const RetainedRgbSession source(processed_linear_gradient(48U, 32U));
    const auto warm = image::prepare_warm_edit_preview(source, 48U);
    const ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    const std::array operations{
        image::LocalMaskComponentOperation::base,
        image::LocalMaskComponentOperation::add,
        image::LocalMaskComponentOperation::subtract,
        image::LocalMaskComponentOperation::intersect,
    };
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "composite",
            .mask = composite_mask(operations, {102U, 102U, 204U, 230U}),
            .nodes = {no_op_node()},
        },
    };
    const auto final = warm.render_rgb8_layers_with_mask_coverage_cancellable(layers, 0U, {});
    const auto selected =
        warm.render_rgb8_layers_with_mask_coverage_cancellable(layers, 0U, {}, {}, nullptr, 2U);
    expect(
        final.completed.has_value() && final.completed->mask_coverage.has_value()
            && selected.completed.has_value() && selected.completed->mask_coverage.has_value(),
        "final and selected composite coverage publish complete paired transactions"
    );
    if (!final.completed || !final.completed->mask_coverage || !selected.completed
        || !selected.completed->mask_coverage) {
        return;
    }
    const auto& final_coverage = *final.completed->mask_coverage;
    const auto& selected_coverage = *selected.completed->mask_coverage;
    expect(
        !final_coverage.component_index.has_value() && selected_coverage.component_index == 2U,
        "coverage identity distinguishes the final composition from a selected leaf"
    );
    expect(
        std::all_of(
            final_coverage.samples.begin(),
            final_coverage.samples.end(),
            [](const auto value) { return value == 51U; }
        )
            && std::all_of(
                selected_coverage.samples.begin(),
                selected_coverage.samples.end(),
                [](const auto value) { return value == 204U; }
            ),
        "Add is idempotent and Subtract/Intersect use the fixed ordered min/max algebra"
    );

    const std::array reordered_layers{
        image::AdjustmentLayer{
            .layer_id = "reordered-composite",
            .mask = composite_mask(
                {
                    image::LocalMaskComponentOperation::base,
                    image::LocalMaskComponentOperation::subtract,
                    image::LocalMaskComponentOperation::add,
                    image::LocalMaskComponentOperation::intersect,
                },
                {102U, 204U, 153U, 230U},
                true
            ),
            .nodes = {no_op_node()},
        },
    };
    const auto reordered =
        warm.render_rgb8_layers_with_mask_coverage_cancellable(reordered_layers, 0U, {});
    expect(
        reordered.completed.has_value() && reordered.completed->mask_coverage.has_value()
            && std::all_of(
                reordered.completed->mask_coverage->samples.begin(),
                reordered.completed->mask_coverage->samples.end(),
                [](const auto value) { return value == 102U; }
            ),
        "component order is authored state and final inversion is applied exactly once"
    );
}

void invalid_target_and_cancellation_fail_closed() {
    const RetainedRgbSession source(processed_linear_gradient());
    const auto warm = image::prepare_warm_edit_preview(source, 96U);
    const ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "mask",
            .mask = representative_masks().front(),
            .nodes = {exposure_node()},
        },
    };
    try {
        static_cast<void>(warm.render_rgb8_layers_with_mask_coverage_cancellable(layers, 1U, {}));
        expect(false, "out-of-range coverage target must throw");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "out-of-range target preserves typed invalid-request semantics"
        );
    }

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled =
        warm.render_rgb8_layers_with_mask_coverage_cancellable(layers, 0U, stop.get_token());
    expect(cancelled.cancelled(), "cancellation publishes neither the preview nor mask half");
}

} // namespace

int main() {
    supported_kinds_publish_geometrically_paired_r8();
    target_semantics_capture_without_mutating_the_frame();
    analyzed_jpeg_keeps_the_same_coverage_transaction();
    composite_coverage_is_idempotent_ordered_and_component_selectable();
    invalid_target_and_cancellation_fail_closed();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
