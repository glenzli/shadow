#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/photo_geometry.hpp>

#include "../../src/edit/local_mask_coverage.hpp"
#include "../../src/proxy/warm_edit_gpu.hpp"
#include "../../src/proxy/warm_edit_gpu_layer_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>

namespace image = shadow::image;

namespace shadow::image::warm_edit_gpu_contract {

namespace {

using parity_fixture::linear_close;
using parity_fixture::make_random_image;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::array<image::LocalMask, 6U> masks() {
    return {
        image::LocalMask{
            .kind = image::LocalMaskKind::linear_gradient,
            .x0 = 0.13,
            .y0 = 0.17,
            .x1 = 0.87,
            .y1 = 0.73,
            .invert = true,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::radial_gradient,
            .x0 = 0.56,
            .y0 = 0.47,
            .radius_x = 0.31,
            .radius_y = 0.25,
            .feather = 0.62,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::brush,
            .radius_x = 0.075,
            .feather = 0.44,
            .points =
                {
                    {.x = 0.14, .y = 0.22, .begins_stroke = true},
                    {.x = 0.46, .y = 0.42},
                    {.x = 0.78, .y = 0.69},
                },
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::luminance_range,
            .x0 = 0.28,
            .x1 = 0.76,
            .feather = 0.09,
        },
        image::LocalMask{
            .kind = image::LocalMaskKind::color_range,
            .x0 = 342.0 / 360.0,
            .x1 = 52.0 / 180.0,
            .feather = 0.41,
            .invert = true,
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

[[nodiscard]] image::LocalMask composite_mask() {
    image::LocalMask composite;
    composite.components = {
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::base,
            .mask = masks()[0U],
        },
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::add,
            .mask = masks()[1U],
        },
    };
    return composite;
}

[[nodiscard]] image::detail::WarmEditGpuRenderContext
geometry_context(const image::FloatRgbImage& source, const image::PhotoGeometry& geometry) {
    const auto layout = image::photo_geometry_layout(source.dimensions, geometry);
    return image::detail::WarmEditGpuRenderContext{
        .adjustment =
            image::AdjustmentExecutionContext{
                .full_dimensions = source.dimensions,
            },
        .geometry = image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect =
                image::GeometryPixelRect{
                    .width = source.dimensions.width,
                    .height = source.dimensions.height,
                },
            .output_rect = image::GeometryPixelRect{
                .width = layout.output_dimensions.width,
                .height = layout.output_dimensions.height,
            },
        },
    };
}

[[nodiscard]] std::uint8_t
maximum_difference(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right) {
    if (left.size() != right.size()) {
        return 255U;
    }
    std::uint8_t maximum = 0U;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        maximum = std::max(
            maximum,
            static_cast<std::uint8_t>(
                std::abs(static_cast<int>(left[index]) - static_cast<int>(right[index]))
            )
        );
    }
    return maximum;
}

[[nodiscard]] image::FloatRgbImage packed_copy(const image::FloatRgbImage& source) {
    image::FloatRgbImage packed{
        .dimensions = source.dimensions,
        .row_stride_bytes = static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float),
        .pixel_format = source.pixel_format,
        .transfer_function = source.transfer_function,
        .reference = source.reference,
        .working_space = source.working_space,
        .level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y,
        .samples =
            std::vector<float>(static_cast<std::size_t>(source.dimensions.pixel_count()) * 3U),
    };
    const std::size_t source_row = source.row_stride_bytes / sizeof(float);
    const std::size_t packed_row = static_cast<std::size_t>(source.dimensions.width) * 3U;
    for (std::uint32_t row = 0U; row < source.dimensions.height; ++row) {
        std::copy_n(
            source.samples.begin() + static_cast<std::ptrdiff_t>(row * source_row),
            packed_row,
            packed.samples.begin() + static_cast<std::ptrdiff_t>(row * packed_row)
        );
    }
    return packed;
}

void composite_masks_are_explicitly_admitted_to_cpu_replay_only() {
    const auto source = make_random_image(41U, 29U, true);
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "ordinary-metal",
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "ordinary-exposure",
                        .parameters = image::ExposureAdjustment{.stops = 0.1},
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "composite-cpu-replay",
            .mask = composite_mask(),
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "composite-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.2},
                },
            },
        },
    };
    const auto plan = image::detail::prepare_warm_gpu_layer_plan(
        source,
        layers,
        image::detail::WarmEditGpuRenderContext{
            .adjustment = image::AdjustmentExecutionContext{.full_dimensions = source.dimensions},
        }
    );
    expect(
        !plan.complete && plan.active_layers.empty()
            && plan.diagnostic.find("exact CPU replay") != std::string::npos,
        "resident Metal explicitly declines composite masks instead of executing a legacy leaf"
    );

    for (const auto& inactive : {
             image::AdjustmentLayer{
                 .layer_id = "disabled-composite",
                 .enabled = false,
                 .mask = composite_mask(),
                 .nodes =
                     {
                         image::AdjustmentNode{
                             .node_id = "disabled-composite-exposure",
                             .parameters = image::ExposureAdjustment{.stops = 0.2},
                         },
                     },
             },
             image::AdjustmentLayer{
                 .layer_id = "zero-opacity-composite",
                 .opacity = 0.0,
                 .mask = composite_mask(),
                 .nodes = {
                     image::AdjustmentNode{
                         .node_id = "zero-opacity-composite-exposure",
                         .parameters = image::ExposureAdjustment{.stops = 0.2},
                     },
                 },
             },
         }) {
        const std::array inactive_layers{layers.front(), inactive};
        const auto inactive_plan = image::detail::prepare_warm_gpu_layer_plan(
            source,
            inactive_layers,
            image::detail::WarmEditGpuRenderContext{
                .adjustment = image::AdjustmentExecutionContext{
                    .full_dimensions = source.dimensions,
                },
            }
        );
        expect(
            inactive_plan.complete && inactive_plan.active_layers.size() == 1U,
            "inactive composite masks do not evict an otherwise Metal-capable render"
        );

        const auto coverage_plan = image::detail::prepare_warm_gpu_layer_plan(
            source,
            inactive_layers,
            image::detail::WarmEditGpuRenderContext{
                .adjustment =
                    image::AdjustmentExecutionContext{
                        .full_dimensions = source.dimensions,
                    },
            },
            1U
        );
        expect(
            !coverage_plan.complete && coverage_plan.active_layers.empty()
                && coverage_plan.diagnostic.find("coverage requires exact CPU replay")
                       != std::string::npos,
            "composite coverage remains an exact CPU replay even when its layer is inactive"
        );
    }
}

void supported_kinds_match_cpu_on_pre_adjustment_input_and_geometry() {
    const auto source = make_random_image(173U, 109U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "mask-coverage Metal parity was required but unavailable"
        );
        return;
    }
    const image::PhotoGeometry geometry{
        .crop_left = 0.07,
        .crop_top = 0.09,
        .crop_right = 0.92,
        .crop_bottom = 0.89,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_270,
        .straighten_degrees = -2.5,
        .flip_vertical = true,
    };
    const auto context = geometry_context(source, geometry);

    for (const auto& mask : masks()) {
        const std::array layers{
            image::AdjustmentLayer{
                .layer_id = "pre-input",
                .opacity = 0.81,
                .nodes =
                    {
                        image::AdjustmentNode{
                            .node_id = "pre-input-exposure",
                            .parameters = image::ExposureAdjustment{.stops = 0.22},
                        },
                    },
            },
            image::AdjustmentLayer{
                .layer_id = "coverage-target",
                .opacity = 0.73,
                .mask = mask,
                .nodes = {
                    image::AdjustmentNode{
                        .node_id = "target-saturation",
                        .parameters = image::SaturationAdjustment{.factor = 0.84},
                    },
                },
            },
        };
        const auto cpu = image::detail::execute_adjustment_layers_with_mask_coverage(
            source,
            layers,
            1U,
            image::AdjustmentExecutionContext{
                .full_dimensions = source.dimensions,
            },
            {}
        );
        expect(
            cpu.has_value() && cpu->mask_coverage.has_value(),
            "CPU oracle captures the target coverage"
        );
        if (!cpu || !cpu->mask_coverage) {
            continue;
        }
        const auto cpu_r8 =
            image::detail::apply_local_mask_coverage_geometry(*cpu->mask_coverage, geometry, {});
        const auto gpu = preparation.session->render_layers(layers, true, context, 1U);
        if (gpu.status != image::detail::WarmEditGpuSession::RenderStatus::completed) {
            std::cerr << "Mask coverage Metal diagnostic: " << gpu.diagnostic << '\n';
        }
        expect(
            gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
                && gpu.output.has_value() && gpu.output->mask_coverage.has_value()
                && gpu.output->analyzed_linear.has_value() && cpu_r8.has_value(),
            "resident Metal completes paired RGB and mask coverage"
        );
        if (!gpu.output || !gpu.output->mask_coverage || !gpu.output->analyzed_linear || !cpu_r8) {
            continue;
        }
        const auto& metal_r8 = *gpu.output->mask_coverage;
        const std::uint8_t difference = maximum_difference(metal_r8.samples, cpu_r8->samples);
        if (difference > 1U) {
            std::cerr << "Mask coverage R8 maximum difference=" << static_cast<unsigned>(difference)
                      << '\n';
        }
        expect(
            metal_r8.dimensions == cpu_r8->dimensions
                && metal_r8.row_stride_bytes == cpu_r8->row_stride_bytes && difference <= 1U,
            "resident Metal coverage tracks the CPU five-kind R8 oracle"
        );
        const auto cpu_geometry = image::apply_photo_geometry(packed_copy(cpu->pixels), geometry);
        double maximum_linear_error = 0.0;
        expect(
            linear_close(*gpu.output->analyzed_linear, cpu_geometry, maximum_linear_error, 1.8e-3),
            "reusing target coverage preserves paired RGB layer parity"
        );
    }
}

void inactive_targets_capture_and_fail_closed() {
    const auto source = make_random_image(121U, 77U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        return;
    }
    const auto mask = masks().front();
    for (const auto& layer : {
             image::AdjustmentLayer{
                 .layer_id = "disabled",
                 .enabled = false,
                 .mask = mask,
                 .nodes =
                     {
                         image::AdjustmentNode{
                             .node_id = "disabled-exposure",
                             .parameters = image::ExposureAdjustment{.stops = 0.4},
                         },
                     },
             },
             image::AdjustmentLayer{
                 .layer_id = "zero-opacity",
                 .opacity = 0.0,
                 .mask = mask,
                 .nodes =
                     {
                         image::AdjustmentNode{
                             .node_id = "zero-opacity-exposure",
                             .parameters = image::ExposureAdjustment{.stops = 0.4},
                         },
                     },
             },
             image::AdjustmentLayer{
                 .layer_id = "no-op",
                 .mask = mask,
                 .nodes = {
                     image::AdjustmentNode{
                         .node_id = "no-op-exposure",
                         .parameters = image::ExposureAdjustment{.stops = 0.0},
                     },
                 },
             },
         }) {
        const std::array layers{layer};
        const auto result =
            preparation.session
                ->render_layers(layers, true, image::detail::WarmEditGpuRenderContext{}, 0U);
        expect(
            result.status == image::detail::WarmEditGpuSession::RenderStatus::completed
                && result.output.has_value() && result.output->mask_coverage.has_value(),
            "disabled, zero-opacity and no-op Metal targets still capture"
        );
    }

    const std::array no_mask{
        image::AdjustmentLayer{
            .layer_id = "no-mask",
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "no-mask-no-op",
                    .parameters = image::ExposureAdjustment{.stops = 0.0},
                },
            },
        },
    };
    const auto absent =
        preparation.session
            ->render_layers(no_mask, false, image::detail::WarmEditGpuRenderContext{}, 0U);
    expect(
        absent.output.has_value() && !absent.output->mask_coverage.has_value(),
        "valid unmasked Metal target publishes no coverage"
    );
    try {
        static_cast<void>(
            preparation.session
                ->render_layers(no_mask, false, image::detail::WarmEditGpuRenderContext{}, 1U)
        );
        expect(false, "out-of-range Metal target must throw");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "Metal out-of-range target is a typed invalid request"
        );
    }

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = preparation.session->render_layers(
        no_mask,
        false,
        image::detail::WarmEditGpuRenderContext{},
        0U,
        stop.get_token()
    );
    expect(
        cancelled.status == image::detail::WarmEditGpuSession::RenderStatus::cancelled
            && !cancelled.output.has_value(),
        "cancelled Metal capture publishes neither paired half"
    );
}

} // namespace

int run_resident_gpu_mask_coverage_contract() {
    composite_masks_are_explicitly_admitted_to_cpu_replay_only();
    supported_kinds_match_cpu_on_pre_adjustment_input_and_geometry();
    inactive_targets_capture_and_fail_closed();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract
