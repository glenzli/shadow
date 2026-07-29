// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu_dispatcher.hpp"

#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
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

        id<MTLBuffer> final_adjusted =
            current == slot.adjusted ? slot.denoised : slot.adjusted;
        [encoder setComputePipelineState:context.display_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:final_adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:slot.before_operations
                    offset:transaction.final_program.operation_offset_bytes
                   atIndex:3U];
        [encoder setBytes:&transaction.final_program.program.invocation
                   length:sizeof(transaction.final_program.program.invocation)
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
        dispatch_warm_gpu_raster(encoder, context.display_pipeline(), resident_layout.dimensions);
        [encoder endEncoding];
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

        RenderResult result{
            .dimensions = resident_layout.dimensions,
            .rgb8 = std::vector<std::uint8_t>(resident_layout.rgb8_bytes),
            .analyzed_linear = std::nullopt,
            .had_active_adjustments = transaction.had_active_adjustments,
        };
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        std::memcpy(result.rgb8.data(), [slot.rgb8 contents], resident_layout.rgb8_bytes);
        if (retain_linear_for_analysis) {
            FloatRgbImage linear{
                .dimensions = resident_layout.dimensions,
                .row_stride_bytes = resident_layout.adjusted_row_stride_bytes,
                .pixel_format = resident_layout.pixel_format,
                .transfer_function = resident_layout.transfer_function,
                .reference = resident_layout.reference,
                .working_space = resident_layout.working_space,
                .level_zero_to_raster_scale_x =
                    resident_layout.level_zero_to_raster_scale_x,
                .level_zero_to_raster_scale_y =
                    resident_layout.level_zero_to_raster_scale_y,
                .samples = std::vector<float>(resident_layout.adjusted_sample_count),
            };
            std::memcpy(
                linear.samples.data(),
                [final_adjusted contents],
                resident_layout.adjusted_bytes
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
