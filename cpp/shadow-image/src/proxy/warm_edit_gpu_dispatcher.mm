// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu_dispatcher.hpp"

#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_render_plan.hpp"
#include "warm_edit_gpu_resident_resources.hpp"
#include "warm_edit_gpu_stage_encoder.hpp"
#include "../edit/metal_adjustment_program.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

struct PreparedWarmProgram final {
    PreparedMetalAdjustment program;
    WarmProgramBuffers buffers;
    std::size_t operation_offset_bytes = 0U;
};

struct PreparedWarmPass final {
    const WarmGpuRenderPass* plan = nullptr;
    PreparedWarmProgram before;
    std::optional<PreparedWarmProgram> post;
};

struct WarmStagePostView final {
    std::span<const AdjustmentNode> nodes;
    const EditExecutionPlan* plan = nullptr;
};

[[nodiscard]] std::optional<WarmStagePostView> stage_post_view(
    const WarmGpuNeighbourhoodStage& stage
) {
    return std::visit(
        [](const auto& value) -> std::optional<WarmStagePostView> {
            using Stage = std::decay_t<decltype(value)>;
            if constexpr (requires(Stage candidate) { candidate.post_nodes; }) {
                if (value.post_nodes.empty()) {
                    return std::nullopt;
                }
                return WarmStagePostView{
                    .nodes = value.post_nodes,
                    .plan = &value.after,
                };
            } else {
                return std::nullopt;
            }
        },
        stage
    );
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
    const FloatRgbImage source_layout{
        .dimensions = resident_layout.dimensions,
        .row_stride_bytes = resident_layout.source_row_stride_bytes,
        .pixel_format = resident_layout.pixel_format,
        .transfer_function = resident_layout.transfer_function,
        .reference = resident_layout.reference,
        .working_space = resident_layout.working_space,
        .level_zero_to_raster_scale_x = resident_layout.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = resident_layout.level_zero_to_raster_scale_y,
        // The immutable source was fully validated before upload. Program preparation needs
        // layout/color metadata, not another full-raster finiteness scan.
        .samples = {},
    };
    std::string preparation_diagnostic;
    const auto prepare_program = [
        &source_layout,
        &resident,
        &resident_layout,
        &preparation_diagnostic,
        &render_context
    ](
        const std::span<const AdjustmentNode> program_nodes,
        const EditExecutionPlan& candidate,
        const std::uint32_t input_row_floats,
        const std::uint32_t output_row_floats
    ) -> std::optional<PreparedMetalAdjustment> {
        PreparedMetalAdjustment result;
        if (candidate.segments.empty()) {
            result.invocation.width = resident_layout.dimensions.width;
            result.invocation.height = resident_layout.dimensions.height;
            result.invocation.step_count = 0U;
        } else {
            auto preparation = prepare_metal_adjustment(
                source_layout,
                program_nodes,
                candidate,
                render_context.adjustment,
                true
            );
            if (!preparation.program.has_value()) {
                preparation_diagnostic = preparation.diagnostic.empty()
                    ? "session-resident Metal could not prepare a pixel-local adjustment segment"
                    : std::move(preparation.diagnostic);
                return std::nullopt;
            }
            result = std::move(*preparation.program);
        }
        result.invocation.input_row_floats = input_row_floats;
        result.invocation.output_row_floats = output_row_floats;
        if (result.operations.size() > resident.operation_capacity()) {
            preparation_diagnostic =
                "session-resident Metal exceeds its 256-operation slot capacity";
            return std::nullopt;
        }
        return result;
    };

    const std::uint32_t source_row_floats =
        static_cast<std::uint32_t>(resident_layout.source_row_stride_bytes / sizeof(float));
    const std::uint32_t packed_row_floats = static_cast<std::uint32_t>(
        resident_layout.adjusted_row_stride_bytes / sizeof(float)
    );
    const WarmGpuRenderPlan render_plan = prepare_warm_gpu_render_plan(
        nodes,
        plan,
        resident_layout.dimensions,
        resident_layout.working_space,
        resident_layout.level_zero_to_raster_scale_x,
        resident_layout.level_zero_to_raster_scale_y
    );
    if (!render_plan.complete) {
        return failed(
            "session-resident Metal cannot lower every neighborhood stage in this adjustment plan"
        );
    }

    std::vector<PreparedWarmPass> prepared_passes;
    prepared_passes.reserve(render_plan.passes.size());
    for (std::size_t index = 0U; index < render_plan.passes.size(); ++index) {
        const WarmGpuRenderPass& pass = render_plan.passes[index];
        auto before = prepare_program(
            nodes,
            pass.before,
            index == 0U ? source_row_floats : packed_row_floats,
            packed_row_floats
        );
        if (!before.has_value()) {
            return failed(std::move(preparation_diagnostic));
        }
        PreparedWarmPass prepared{
            .plan = &pass,
            .before = PreparedWarmProgram{.program = std::move(*before)},
        };
        if (const auto post = stage_post_view(pass.neighbourhood); post.has_value()) {
            auto post_program = prepare_program(
                post->nodes,
                *post->plan,
                packed_row_floats,
                packed_row_floats
            );
            if (!post_program.has_value()) {
                return failed(std::move(preparation_diagnostic));
            }
            prepared.post.emplace(
                PreparedWarmProgram{.program = std::move(*post_program)}
            );
        }
        prepared_passes.push_back(std::move(prepared));
    }
    auto final_preparation = prepare_program(
        nodes,
        render_plan.after,
        render_plan.passes.empty() ? source_row_floats : packed_row_floats,
        packed_row_floats
    );
    if (!final_preparation.has_value()) {
        return failed(std::move(preparation_diagnostic));
    }
    PreparedWarmProgram final_program{
        .program = std::move(*final_preparation),
    };

    std::vector<PreparedWarmProgram*> programs;
    programs.reserve(prepared_passes.size() * 2U + 1U);
    for (PreparedWarmPass& pass : prepared_passes) {
        programs.push_back(&pass.before);
        if (pass.post.has_value()) {
            programs.push_back(&*pass.post);
        }
    }
    programs.push_back(&final_program);

    std::size_t operation_count = 0U;
    for (PreparedWarmProgram* program : programs) {
        if (program->program.operations.size() > resident.operation_capacity() - operation_count) {
            return failed(
                "session-resident Metal composed plan exceeds its 256-operation slot capacity"
            );
        }
        program->operation_offset_bytes = operation_count * sizeof(MetalAdjustmentOp);
        operation_count += program->program.operations.size();

        auto attempt = resident.acquire_program_buffers(program->program, cancellation);
        if (attempt.cancelled) {
            return cancelled();
        }
        if (!attempt.diagnostic.empty()) {
            return failed(std::move(attempt.diagnostic));
        }
        program->buffers = std::move(attempt.buffers);
    }
    if (cancellation.stop_requested()) {
        return cancelled();
    }

    auto slot_lease = resident.acquire_slot(cancellation);
    if (!slot_lease.has_value()) {
        return cancelled();
    }
    for (const PreparedWarmPass& pass : prepared_passes) {
        const std::string diagnostic =
            ensure_warm_gpu_stage_resources(*slot_lease, pass.plan->neighbourhood);
        if (!diagnostic.empty()) {
            return failed(diagnostic);
        }
    }
    const WarmGpuSlotBuffers slot = slot_lease->buffers();

    @autoreleasepool {
        auto* operation_bytes = static_cast<std::byte*>([slot.before_operations contents]);
        for (const PreparedWarmProgram* program : programs) {
            const std::size_t bytes =
                program->program.operations.size() * sizeof(MetalAdjustmentOp);
            if (bytes > 0U) {
                std::memcpy(
                    operation_bytes + program->operation_offset_bytes,
                    program->program.operations.data(),
                    bytes
                );
            }
        }
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
        const auto dispatch = [encoder, &resident_layout](
            id<MTLComputePipelineState> pipeline
        ) {
            const NSUInteger thread_width = std::min<NSUInteger>(
                32U,
                std::max<NSUInteger>(1U, pipeline.threadExecutionWidth)
            );
            const NSUInteger thread_height = std::max<NSUInteger>(
                1U,
                std::min<NSUInteger>(
                    8U,
                    pipeline.maxTotalThreadsPerThreadgroup / thread_width
                )
            );
            [encoder dispatchThreads:MTLSizeMake(
                    resident_layout.dimensions.width,
                    resident_layout.dimensions.height,
                    1U
                )
                threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
        };
        const auto bind_adjustment = [&encoder, &slot](
            id<MTLBuffer> input,
            id<MTLBuffer> output,
            const PreparedWarmProgram& prepared
        ) {
            [encoder setBuffer:input offset:0U atIndex:0U];
            [encoder setBuffer:output offset:0U atIndex:1U];
            [encoder setBuffer:slot.before_operations
                        offset:prepared.operation_offset_bytes
                       atIndex:3U];
            [encoder setBytes:&prepared.program.invocation
                       length:sizeof(prepared.program.invocation)
                      atIndex:4U];
            [encoder setBuffer:slot.status offset:0U atIndex:6U];
            [encoder setBuffer:prepared.buffers.curve.get() offset:0U atIndex:7U];
            [encoder setBuffer:prepared.buffers.lut.get() offset:0U atIndex:8U];
            [encoder setBuffer:prepared.buffers.perceptual_mixer.get()
                        offset:0U
                       atIndex:9U];
            [encoder setBuffer:prepared.buffers.perceptual_range.get()
                        offset:0U
                       atIndex:10U];
            [encoder setBuffer:prepared.buffers.selective_color.get()
                        offset:0U
                       atIndex:11U];
        };

        id<MTLBuffer> current = resident.source_buffer();
        for (const PreparedWarmPass& pass : prepared_passes) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            // Pixel-local kernels load one pixel completely before writing it. Keeping a
            // previous stage's adjusted buffer in place is therefore safe and avoids an
            // otherwise redundant full-raster copy between adjacent neighborhood stages.
            bind_adjustment(current, slot.adjusted, pass.before);
            dispatch(context.adjustment_pipeline());
            current = encode_warm_gpu_neighbourhood_stage(
                encoder,
                context,
                resident_layout,
                slot,
                slot.adjusted,
                pass.plan->neighbourhood,
                pass.post.has_value()
                    ? pass.post->program.invocation
                    : pass.before.program.invocation
            );
            if (pass.post.has_value()) {
                [encoder setComputePipelineState:context.adjustment_pipeline()];
                bind_adjustment(current, current, *pass.post);
                dispatch(context.adjustment_pipeline());
            }
        }

        id<MTLBuffer> final_adjusted =
            current == slot.adjusted ? slot.denoised : slot.adjusted;
        [encoder setComputePipelineState:context.display_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:final_adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:slot.before_operations
                    offset:final_program.operation_offset_bytes
                   atIndex:3U];
        [encoder setBytes:&final_program.program.invocation
                   length:sizeof(final_program.program.invocation)
                  atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];
        [encoder setBuffer:final_program.buffers.curve.get() offset:0U atIndex:7U];
        [encoder setBuffer:final_program.buffers.lut.get() offset:0U atIndex:8U];
        [encoder setBuffer:final_program.buffers.perceptual_mixer.get()
                    offset:0U
                   atIndex:9U];
        [encoder setBuffer:final_program.buffers.perceptual_range.get()
                    offset:0U
                   atIndex:10U];
        [encoder setBuffer:final_program.buffers.selective_color.get()
                    offset:0U
                   atIndex:11U];
        dispatch(context.display_pipeline());
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
            for (const PreparedWarmProgram* program : programs) {
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
            .had_active_adjustments = !plan.segments.empty(),
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
