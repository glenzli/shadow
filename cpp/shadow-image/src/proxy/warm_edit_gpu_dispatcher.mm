// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu_dispatcher.hpp"

#include "warm_edit_gpu_geometry_encoder.hpp"
#include "warm_edit_gpu_geometry_plan.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_presentation_surface.hpp"
#include "warm_edit_gpu_resident_resources.hpp"
#include "warm_edit_gpu_transaction.hpp"
#include "warm_edit_gpu_transaction_encoder.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

} // namespace

WarmEditGpuSession::RenderAttempt dispatch_warm_edit_gpu(
    WarmGpuResidentResources& resident,
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis,
    const WarmEditGpuRenderContext render_context,
    const std::stop_token cancellation
) {
    using RenderAttempt = WarmEditGpuSession::RenderAttempt;
    using RenderResult = WarmEditGpuSession::RenderResult;
    using RenderStatus = WarmEditGpuSession::RenderStatus;

    const auto cancelled = [] {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    };
    const auto failed = [](std::string diagnostic) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = std::move(diagnostic),
        };
    };
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (force_test_failure()) {
        return failed("test-injected session-resident Metal warm-preview failure");
    }

    const WarmGpuResidentLayout& resident_layout = resident.layout();
    const std::uint32_t source_row_floats =
        static_cast<std::uint32_t>(resident_layout.source_row_stride_bytes / sizeof(float));
    std::optional<WarmGpuGeometryPlan> geometry_plan;
    RetainedMetalBuffer geometry_liquify_buffer;
    if (render_context.geometry.has_value()) {
        auto geometry = prepare_warm_gpu_geometry_plan(
            resident_layout.dimensions,
            source_row_floats,
            resident_layout.level_zero_to_raster_scale_x,
            resident_layout.level_zero_to_raster_scale_y,
            *render_context.geometry
        );
        if (!geometry.plan.has_value()) {
            return failed(
                geometry.diagnostic.empty()
                    ? "session-resident Metal photo geometry is unavailable"
                    : std::move(geometry.diagnostic)
            );
        }
        geometry_plan = std::move(*geometry.plan);
        auto buffer = resident.acquire_liquify_geometry_buffer(
            geometry_plan->liquify_words,
            cancellation
        );
        if (buffer.cancelled) {
            return cancelled();
        }
        if (!buffer.buffer) {
            return failed(
                buffer.diagnostic.empty()
                    ? "session-resident Metal has no photo Liquify side table"
                    : std::move(buffer.diagnostic)
            );
        }
        geometry_liquify_buffer = std::move(buffer.buffer);
    }
    std::size_t operation_count = 0U;
    auto preparation = prepare_warm_gpu_transaction(
        resident,
        nodes,
        plan,
        render_context,
        source_row_floats,
        operation_count,
        cancellation
    );
    if (preparation.cancelled) {
        return cancelled();
    }
    if (!preparation.transaction.has_value()) {
        return failed(std::move(preparation.diagnostic));
    }
    PreparedWarmTransaction transaction = std::move(*preparation.transaction);
    if (cancellation.stop_requested()) {
        return cancelled();
    }

    auto slot_lease = resident.acquire_slot(cancellation);
    if (!slot_lease.has_value()) {
        return cancelled();
    }
    const std::string resource_diagnostic =
        ensure_warm_gpu_transaction_resources(*slot_lease, transaction);
    if (!resource_diagnostic.empty()) {
        return failed(resource_diagnostic);
    }
    if (geometry_plan.has_value()) {
        const std::string geometry_diagnostic = slot_lease->ensure_denoise_resources();
        if (!geometry_diagnostic.empty()) {
            return failed(geometry_diagnostic);
        }
    }
    const WarmGpuSlotBuffers slot = slot_lease->buffers();

    @autoreleasepool {
        copy_warm_gpu_transaction_operations(slot.before_operations, transaction);
        auto* status = static_cast<WarmStatus*>([slot.status contents]);
        *status = WarmStatus{};
        const WarmDisplayParameters display{
            .output_origin_x = render_context.display_origin_x,
            .output_origin_y = render_context.display_origin_y,
            .apply_scene_curve =
                resident_layout.reference == ImageReference::scene_referred ? 1U : 0U,
            .retain_linear = retain_linear_for_analysis ? 1U : 0U,
        };

        WarmMetalContext& context = metal_context();
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return failed("Metal could not create a warm-preview compute command");
        }
        id<MTLBuffer> current = encode_warm_gpu_transaction_prefix(
            encoder,
            context,
            resident_layout,
            slot,
            resident.source_buffer(),
            transaction
        );

        Dimensions output_dimensions = resident_layout.dimensions;
        std::size_t output_sample_count = resident_layout.adjusted_sample_count;
        std::size_t output_linear_bytes = resident_layout.adjusted_bytes;
        std::size_t output_rgb8_bytes = resident_layout.rgb8_bytes;
        double output_scale_x = resident_layout.level_zero_to_raster_scale_x;
        double output_scale_y = resident_layout.level_zero_to_raster_scale_y;
        auto display_invocation = transaction.final_program.program.invocation;
        if (geometry_plan.has_value()) {
            if (!transaction.final_program.program.operations.empty()) {
                id<MTLBuffer> materialized =
                    current == slot.denoised ? slot.denoised : slot.adjusted;
                [encoder setComputePipelineState:context.adjustment_pipeline()];
                bind_warm_gpu_adjustment(
                    encoder,
                    slot,
                    current,
                    materialized,
                    transaction.final_program
                );
                dispatch_warm_gpu_raster(
                    encoder,
                    context.adjustment_pipeline(),
                    resident_layout.dimensions
                );
                current = materialized;
            }
            id<MTLBuffer> geometry_output =
                current == slot.adjusted ? slot.denoised : slot.adjusted;
            const std::uint32_t geometry_input_row_floats =
                current == resident.source_buffer()
                ? source_row_floats
                : static_cast<std::uint32_t>(
                    resident_layout.adjusted_row_stride_bytes / sizeof(float)
                );
            encode_warm_gpu_geometry(
                encoder,
                context,
                current,
                geometry_output,
                slot.status,
                geometry_liquify_buffer.get(),
                geometry_input_row_floats,
                *geometry_plan
            );
            current = geometry_output;
            output_dimensions = geometry_plan->output_dimensions;
            output_sample_count =
                static_cast<std::size_t>(output_dimensions.pixel_count()) * 3U;
            output_linear_bytes = output_sample_count * sizeof(float);
            output_rgb8_bytes = output_sample_count;
            output_scale_x = geometry_plan->output_level_zero_to_raster_scale_x;
            output_scale_y = geometry_plan->output_level_zero_to_raster_scale_y;
            display_invocation.width = output_dimensions.width;
            display_invocation.height = output_dimensions.height;
            display_invocation.input_row_floats = output_dimensions.width * 3U;
            display_invocation.output_row_floats = output_dimensions.width * 3U;
            display_invocation.step_count = 0U;
            display_invocation.curve_segment_count = 0U;
            display_invocation.lut_entry_count = 0U;
            display_invocation.perceptual_mixer_entry_count = 0U;
            display_invocation.perceptual_range_entry_count = 0U;
            display_invocation.selective_color_entry_count = 0U;
        }
        id<MTLBuffer> final_adjusted =
            current == slot.adjusted ? slot.denoised : slot.adjusted;
        [encoder setComputePipelineState:context.display_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:final_adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:slot.before_operations
                    offset:transaction.final_program.operation_offset_bytes
                   atIndex:3U];
        [encoder setBytes:&display_invocation
                   length:sizeof(display_invocation)
                  atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];
        [encoder setBuffer:transaction.final_program.buffers.curve.get()
                    offset:0U
                   atIndex:7U];
        [encoder setBuffer:transaction.final_program.buffers.lut.get()
                    offset:0U
                   atIndex:8U];
        [encoder setBuffer:transaction.final_program.buffers.perceptual_mixer.get()
                    offset:0U
                   atIndex:9U];
        [encoder setBuffer:transaction.final_program.buffers.perceptual_range.get()
                    offset:0U
                   atIndex:10U];
        [encoder setBuffer:transaction.final_program.buffers.selective_color.get()
                    offset:0U
                   atIndex:11U];
        dispatch_warm_gpu_raster(encoder, context.display_pipeline(), output_dimensions);
        [encoder endEncoding];

        std::shared_ptr<WarmEditGpuPresentationSurface> presentation_surface;
        std::string presentation_fallback_diagnostic;
        if (
            render_context.output_intent
                == WarmEditGpuOutputIntent::metal_presentation_surface
            && !retain_linear_for_analysis
        ) {
            resident.record_presentation_surface_request();
            auto preparation_surface =
                prepare_warm_edit_gpu_presentation_surface(
                    context.device(),
                    output_dimensions
                );
            if (preparation_surface.surface) {
                const std::string presentation_diagnostic =
                    encode_warm_edit_gpu_presentation_surface(
                        command_buffer,
                        slot.rgb8,
                        *preparation_surface.surface
                );
                if (presentation_diagnostic.empty()) {
                    presentation_surface =
                        std::move(preparation_surface.surface);
                } else {
                    presentation_fallback_diagnostic = presentation_diagnostic;
                }
            } else {
                presentation_fallback_diagnostic =
                    preparation_surface.diagnostic.empty()
                    ? "Metal presentation surface preparation was unavailable"
                    : std::move(preparation_surface.diagnostic);
            }
        }
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return failed(command_buffer_diagnostic(command_buffer));
        }
        if (status->flags != 0U) {
            std::string diagnostic =
                "session-resident Metal warm preview produced an invalid result";
            for (const PreparedWarmProgram* program :
                 warm_gpu_transaction_programs(transaction)) {
                if (status->earliest_step < program->program.operations.size()) {
                    diagnostic += " near source node " + std::to_string(
                        program->program.operations[status->earliest_step].source_node_index
                    );
                    break;
                }
            }
            return failed(std::move(diagnostic));
        }
        if (presentation_surface) {
            resident.record_presentation_surface_publish();
        } else if (!presentation_fallback_diagnostic.empty()) {
            resident.record_presentation_surface_fallback();
        }

        RenderResult result{
            .dimensions = output_dimensions,
            .rgb8 = presentation_surface
                ? std::vector<std::uint8_t>{}
                : std::vector<std::uint8_t>(output_rgb8_bytes),
            .presentation_surface = std::move(presentation_surface),
            .presentation_fallback_diagnostic =
                std::move(presentation_fallback_diagnostic),
            .analyzed_linear = std::nullopt,
            .had_active_adjustments =
                transaction.had_active_adjustments || geometry_plan.has_value(),
        };
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        if (!result.presentation_surface) {
            std::memcpy(result.rgb8.data(), [slot.rgb8 contents], output_rgb8_bytes);
        }
        if (retain_linear_for_analysis) {
            FloatRgbImage linear{
                .dimensions = output_dimensions,
                .row_stride_bytes =
                    static_cast<std::size_t>(output_dimensions.width) * 3U * sizeof(float),
                .pixel_format = resident_layout.pixel_format,
                .transfer_function = resident_layout.transfer_function,
                .reference = resident_layout.reference,
                .working_space = resident_layout.working_space,
                .level_zero_to_raster_scale_x = output_scale_x,
                .level_zero_to_raster_scale_y = output_scale_y,
                .samples = std::vector<float>(output_sample_count),
            };
            std::memcpy(
                linear.samples.data(),
                [final_adjusted contents],
                output_linear_bytes
            );
            result.analyzed_linear = std::move(linear);
        }
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        slot_lease->mark_completed();
        return RenderAttempt{
            .status = RenderStatus::completed,
            .output = std::move(result),
            .diagnostic = {},
        };
    }
}

} // namespace shadow::image::detail
