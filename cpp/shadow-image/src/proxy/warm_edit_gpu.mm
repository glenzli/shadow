// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_resident_resources.hpp"
#include "warm_edit_gpu_render_plan.hpp"
#include "../edit/metal_adjustment_program.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

} // namespace

struct WarmEditGpuSession::Impl final {
    std::unique_ptr<WarmGpuResidentResources> resident;
};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) const {
    const auto cancelled = [] {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    };
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (!impl_ || !impl_->resident) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }
    if (force_test_failure()) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "test-injected session-resident Metal warm-preview failure",
        };
    }

    auto& resident = *impl_->resident;
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
        // The immutable source was fully validated before upload. Warm parameter preparation
        // needs its layout/color metadata, not another full-raster finiteness scan.
        .samples = {},
    };
    std::string preparation_diagnostic;
    const auto prepare_program = [
        &source_layout,
        &resident,
        &resident_layout,
        &preparation_diagnostic
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
                AdjustmentExecutionContext{.full_dimensions = resident_layout.dimensions},
                true
            );
            if (!preparation.program.has_value()) {
                preparation_diagnostic = preparation.diagnostic.empty()
                    ? "session-resident Metal warm preview could not prepare the adjustment plan"
                    : std::move(preparation.diagnostic);
                return std::nullopt;
            }
            result = std::move(*preparation.program);
        }
        result.invocation.input_row_floats = input_row_floats;
        result.invocation.output_row_floats = output_row_floats;
        if (result.operations.size() > resident.operation_capacity()) {
            preparation_diagnostic =
                "session-resident Metal warm preview exceeds its 256-operation slot capacity";
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
    const auto* technical_detail_stage = std::get_if<WarmTechnicalDetailStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* texture_clarity_stage = std::get_if<WarmTextureClarityStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* local_contrast_stage = std::get_if<WarmLocalContrastStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* texture_stage = std::get_if<WarmTextureStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* clarity_stage = std::get_if<WarmClarityStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* dehaze_defringe_stage = std::get_if<WarmDehazeDefringeStage>(
        &render_plan.neighbourhood_stage
    );
    const bool has_neighbourhood_stage = render_plan.has_neighbourhood_stage();
    PreparedMetalAdjustment before_program;
    std::optional<PreparedMetalAdjustment> final_program;
    if (technical_detail_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            technical_detail_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            nodes,
            technical_detail_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else if (texture_clarity_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            texture_clarity_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            texture_clarity_stage->post_nodes,
            texture_clarity_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = std::move(preparation_diagnostic)};
        }
        before_program = std::move(*prepared_before);
    } else if (local_contrast_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            local_contrast_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            local_contrast_stage->post_nodes,
            local_contrast_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = std::move(preparation_diagnostic)};
        }
        before_program = std::move(*prepared_before);
    } else if (texture_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            texture_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            texture_stage->post_nodes,
            texture_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else if (clarity_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            clarity_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            clarity_stage->post_nodes,
            clarity_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else if (dehaze_defringe_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            dehaze_defringe_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            dehaze_defringe_stage->post_nodes,
            dehaze_defringe_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else {
        final_program = prepare_program(nodes, plan, source_row_floats, packed_row_floats);
        if (!final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
    }

    if (cancellation.stop_requested()) {
        return cancelled();
    }
    std::optional<WarmProgramBuffers> before_buffers;
    if (has_neighbourhood_stage) {
        auto attempt = resident.acquire_program_buffers(before_program, cancellation);
        if (attempt.cancelled) {
            return cancelled();
        }
        if (!attempt.diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(attempt.diagnostic),
            };
        }
        before_buffers.emplace(std::move(attempt.buffers));
    }
    auto final_buffers_attempt = resident.acquire_program_buffers(*final_program, cancellation);
    if (final_buffers_attempt.cancelled) {
        return cancelled();
    }
    if (!final_buffers_attempt.diagnostic.empty()) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = std::move(final_buffers_attempt.diagnostic),
        };
    }
    WarmProgramBuffers final_buffers = std::move(final_buffers_attempt.buffers);
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    auto slot_lease = resident.acquire_slot(cancellation);
    if (!slot_lease.has_value()) {
        return cancelled();
    }
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (technical_detail_stage != nullptr) {
        const std::string diagnostic = technical_detail_stage->sharpen.has_value()
            ? slot_lease->ensure_sharpen_resources()
            : slot_lease->ensure_denoise_resources();
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (texture_clarity_stage != nullptr) {
        const std::string diagnostic = slot_lease->ensure_texture_clarity_resources();
        if (!diagnostic.empty()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = diagnostic};
        }
    } else if (local_contrast_stage != nullptr) {
        const std::string diagnostic = slot_lease->ensure_local_contrast_resources();
        if (!diagnostic.empty()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = diagnostic};
        }
    } else if (texture_stage != nullptr) {
        const std::string diagnostic = slot_lease->ensure_sharpen_resources();
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (clarity_stage != nullptr) {
        const std::string diagnostic = slot_lease->ensure_clarity_resources();
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (dehaze_defringe_stage != nullptr) {
        const std::string diagnostic = slot_lease->ensure_denoise_resources();
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    }
    const WarmGpuSlotBuffers slot = slot_lease->buffers();

    @autoreleasepool {
        const auto upload_operations = [](id<MTLBuffer> destination,
                                          const PreparedMetalAdjustment& program) {
            const std::size_t bytes = program.operations.size()
                * sizeof(MetalAdjustmentOp);
            if (bytes > 0U) {
                std::memcpy([destination contents], program.operations.data(), bytes);
            }
        };
        if (has_neighbourhood_stage) {
            upload_operations(slot.before_operations, before_program);
            upload_operations(slot.after_operations, *final_program);
        } else {
            upload_operations(slot.before_operations, *final_program);
        }
        auto* status = static_cast<WarmStatus*>([slot.status contents]);
        *status = WarmStatus{};
        const WarmDisplayParameters display{
            .apply_scene_curve =
                resident_layout.reference == ImageReference::scene_referred ? 1U : 0U,
            .retain_linear = retain_linear_for_analysis ? 1U : 0U,
        };

        auto& context = metal_context();
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = "Metal could not create a warm-preview compute command",
            };
        }
        const auto dispatch = [
            encoder,
            &resident_layout
        ](id<MTLComputePipelineState> pipeline) {
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
            id<MTLBuffer> operations,
            const PreparedMetalAdjustment& program,
            const WarmProgramBuffers& buffers
        ) {
            [encoder setBuffer:input offset:0U atIndex:0U];
            [encoder setBuffer:output offset:0U atIndex:1U];
            [encoder setBuffer:operations offset:0U atIndex:3U];
            [encoder setBytes:&program.invocation
                       length:sizeof(program.invocation)
                      atIndex:4U];
            [encoder setBuffer:slot.status offset:0U atIndex:6U];
            [encoder setBuffer:buffers.curve.get() offset:0U atIndex:7U];
            [encoder setBuffer:buffers.lut.get() offset:0U atIndex:8U];
            [encoder setBuffer:buffers.perceptual_mixer.get() offset:0U atIndex:9U];
            [encoder setBuffer:buffers.perceptual_range.get() offset:0U atIndex:10U];
            [encoder setBuffer:buffers.selective_color.get() offset:0U atIndex:11U];
        };

        id<MTLBuffer> neighbourhood_output = resident.source_buffer();
        if (technical_detail_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                resident.source_buffer(),
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());
            neighbourhood_output = slot.adjusted;

            if (technical_detail_stage->denoise.has_value()) {
                const auto& denoise = *technical_detail_stage->denoise;
                [encoder setComputePipelineState:context.denoise_pipeline()];
                [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
                [encoder setBuffer:slot.denoised offset:0U atIndex:1U];
                [encoder setBytes:&denoise length:sizeof(denoise) atIndex:2U];
                dispatch(context.denoise_pipeline());
                neighbourhood_output = slot.denoised;
                if (denoise.passes > 1U) {
                    [encoder setBuffer:slot.denoised offset:0U atIndex:0U];
                    [encoder setBuffer:slot.adjusted offset:0U atIndex:1U];
                    [encoder setBytes:&denoise length:sizeof(denoise) atIndex:2U];
                    dispatch(context.denoise_pipeline());
                    neighbourhood_output = slot.adjusted;
                }
            }

            if (technical_detail_stage->sharpen.has_value()) {
                const auto& sharpen = *technical_detail_stage->sharpen;
                [encoder setComputePipelineState:context.sharpen_log_pipeline()];
                [encoder setBuffer:neighbourhood_output offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
                [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:2U];
                dispatch(context.sharpen_log_pipeline());

                [encoder setComputePipelineState:context.sharpen_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:2U];
                dispatch(context.sharpen_horizontal_pipeline());

                const id<MTLBuffer> sharpened_output = neighbourhood_output == slot.adjusted
                    ? slot.denoised
                    : slot.adjusted;
                [encoder setComputePipelineState:context.sharpen_apply_pipeline()];
                [encoder setBuffer:neighbourhood_output offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBuffer:sharpened_output offset:0U atIndex:2U];
                [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:3U];
                dispatch(context.sharpen_apply_pipeline());
                neighbourhood_output = sharpened_output;
            }
        } else if (texture_clarity_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(resident.source_buffer(), slot.adjusted, slot.before_operations,
                            before_program, *before_buffers);
            dispatch(context.adjustment_pipeline());

            const auto& texture = texture_clarity_stage->texture_gaussian;
            const auto& small = texture_clarity_stage->clarity_small_gaussian;
            const auto& large = texture_clarity_stage->clarity_large_gaussian;
            const auto& combined = texture_clarity_stage->parameters;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            [encoder setBytes:&final_program->invocation length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());
            [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            dispatch(context.scalar_vertical_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());
            [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.scalar_vertical_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&large length:sizeof(large) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());
            [encoder setComputePipelineState:context.texture_clarity_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:1U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:2U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:3U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:4U];
            [encoder setBytes:&combined length:sizeof(combined) atIndex:5U];
            [encoder setBytes:&final_program->invocation length:sizeof(final_program->invocation)
                      atIndex:6U];
            dispatch(context.texture_clarity_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (local_contrast_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                resident.source_buffer(),
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& small = local_contrast_stage->small_box;
            const auto& large = local_contrast_stage->large_box;
            const auto& small_coefficients = local_contrast_stage->small_coefficients;
            const auto& large_coefficients = local_contrast_stage->large_coefficients;
            const auto& local_contrast = local_contrast_stage->parameters;
            const WarmTextureParameters lightness{
                .width = resident_layout.dimensions.width,
                .height = resident_layout.dimensions.height,
            };
            const auto box_mean = [&encoder, &context, &dispatch](
                id<MTLBuffer> input,
                id<MTLBuffer> horizontal,
                id<MTLBuffer> output,
                const WarmBoxParameters& parameters
            ) {
                [encoder setComputePipelineState:context.box_horizontal_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:horizontal offset:0U atIndex:1U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                dispatch(context.box_horizontal_pipeline());
                [encoder setComputePipelineState:context.box_vertical_pipeline()];
                [encoder setBuffer:horizontal offset:0U atIndex:0U];
                [encoder setBuffer:output offset:0U atIndex:1U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                dispatch(context.box_vertical_pipeline());
            };
            const auto square = [&encoder, &context, &dispatch](
                id<MTLBuffer> input,
                id<MTLBuffer> output,
                const WarmBoxParameters& parameters
            ) {
                [encoder setComputePipelineState:context.scalar_square_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:output offset:0U atIndex:1U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                dispatch(context.scalar_square_pipeline());
            };
            const auto coefficients = [&encoder, &context, &dispatch](
                id<MTLBuffer> guide,
                id<MTLBuffer> mean,
                id<MTLBuffer> variance,
                id<MTLBuffer> a,
                id<MTLBuffer> b,
                const WarmGuidedCoefficientsParameters& parameters
            ) {
                [encoder setComputePipelineState:context.guided_coefficients_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:mean offset:0U atIndex:1U];
                [encoder setBuffer:variance offset:0U atIndex:2U];
                [encoder setBuffer:a offset:0U atIndex:3U];
                [encoder setBuffer:b offset:0U atIndex:4U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5U];
                dispatch(context.guided_coefficients_pipeline());
            };
            const auto combine = [&encoder, &context, &dispatch](
                id<MTLBuffer> guide,
                id<MTLBuffer> a,
                id<MTLBuffer> b,
                id<MTLBuffer> output,
                const WarmBoxParameters& parameters
            ) {
                [encoder setComputePipelineState:context.guided_combine_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:a offset:0U atIndex:1U];
                [encoder setBuffer:b offset:0U atIndex:2U];
                [encoder setBuffer:output offset:0U atIndex:3U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4U];
                dispatch(context.guided_combine_pipeline());
            };

            // Scalar allocation layout:
            // guide=A, rolling horizontal=B, small output=C, large output=D,
            // coefficient scratch=E/F. Each phase overwrites only data whose
            // final use has passed, retaining both guided outputs for the
            // final broad-band residual.
            const id<MTLBuffer> guide = slot.sharpen_log_luminance;
            const id<MTLBuffer> horizontal = slot.sharpen_horizontal;
            const id<MTLBuffer> small_output = slot.perceptual_small;
            const id<MTLBuffer> large_output = slot.perceptual_texture;
            const id<MTLBuffer> scratch_a = slot.local_contrast_a;
            const id<MTLBuffer> scratch_b = slot.local_contrast_b;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:guide offset:0U atIndex:1U];
            [encoder setBytes:&lightness length:sizeof(lightness) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation) atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            box_mean(guide, horizontal, small_output, small);
            square(guide, horizontal, small);
            box_mean(horizontal, scratch_a, large_output, small);
            coefficients(
                guide,
                small_output,
                large_output,
                scratch_a,
                scratch_b,
                small_coefficients
            );
            box_mean(scratch_a, horizontal, small_output, small);
            box_mean(scratch_b, horizontal, large_output, small);
            combine(guide, small_output, large_output, small_output, small);

            box_mean(guide, horizontal, large_output, large);
            square(guide, horizontal, large);
            box_mean(horizontal, scratch_a, scratch_b, large);
            coefficients(
                guide,
                large_output,
                scratch_b,
                scratch_a,
                horizontal,
                large_coefficients
            );
            box_mean(scratch_a, scratch_b, scratch_a, large);
            box_mean(horizontal, scratch_b, large_output, large);
            combine(guide, scratch_a, large_output, large_output, large);

            [encoder setComputePipelineState:context.local_contrast_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:small_output offset:0U atIndex:1U];
            [encoder setBuffer:large_output offset:0U atIndex:2U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:3U];
            [encoder setBytes:&local_contrast length:sizeof(local_contrast) atIndex:4U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation) atIndex:5U];
            dispatch(context.local_contrast_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (texture_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                resident.source_buffer(),
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& texture = texture_stage->parameters;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());

            [encoder setComputePipelineState:context.texture_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:2U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:3U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:4U];
            dispatch(context.texture_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (clarity_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                resident.source_buffer(),
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& small = clarity_stage->small_gaussian;
            const auto& large = clarity_stage->large_gaussian;
            const auto& clarity = clarity_stage->parameters;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());

            [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.scalar_vertical_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&large length:sizeof(large) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());

            [encoder setComputePipelineState:context.clarity_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:2U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:3U];
            [encoder setBytes:&clarity length:sizeof(clarity) atIndex:4U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:5U];
            dispatch(context.clarity_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (dehaze_defringe_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                resident.source_buffer(),
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& technical_optics = dehaze_defringe_stage->parameters;
            [encoder setComputePipelineState:context.dehaze_defringe_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:1U];
            [encoder setBytes:&technical_optics length:sizeof(technical_optics) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.dehaze_defringe_pipeline());
            neighbourhood_output = slot.denoised;
        }

        [encoder setComputePipelineState:context.display_pipeline()];
        id<MTLBuffer> final_input = neighbourhood_output;
        id<MTLBuffer> final_adjusted = has_neighbourhood_stage
            ? (final_input == slot.adjusted ? slot.denoised : slot.adjusted)
            : slot.adjusted;
        id<MTLBuffer> final_operations = has_neighbourhood_stage
            ? slot.after_operations
            : slot.before_operations;
        [encoder setBuffer:final_input offset:0U atIndex:0U];
        [encoder setBuffer:final_adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:final_operations offset:0U atIndex:3U];
        [encoder setBytes:&final_program->invocation
                   length:sizeof(final_program->invocation)
                  atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];
        [encoder setBuffer:final_buffers.curve.get() offset:0U atIndex:7U];
        [encoder setBuffer:final_buffers.lut.get() offset:0U atIndex:8U];
        [encoder setBuffer:final_buffers.perceptual_mixer.get() offset:0U atIndex:9U];
        [encoder setBuffer:final_buffers.perceptual_range.get() offset:0U atIndex:10U];
        [encoder setBuffer:final_buffers.selective_color.get() offset:0U atIndex:11U];
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
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = command_buffer_diagnostic(command_buffer),
            };
        }
        if (status->flags != 0U) {
            std::string diagnostic =
                "session-resident Metal warm preview produced an invalid result";
            const auto append_node = [&diagnostic, status](
                const PreparedMetalAdjustment& program
            ) {
                if (status->earliest_step < program.operations.size()) {
                    diagnostic += " at source node " + std::to_string(
                        program.operations[status->earliest_step].source_node_index
                    );
                }
            };
            if (has_neighbourhood_stage) {
                append_node(before_program);
                append_node(*final_program);
            } else {
                append_node(*final_program);
            }
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(diagnostic),
            };
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
                .level_zero_to_raster_scale_x = resident_layout.level_zero_to_raster_scale_x,
                .level_zero_to_raster_scale_y = resident_layout.level_zero_to_raster_scale_y,
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

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    if (!impl_ || !impl_->resident) {
        return {};
    }
    return impl_->resident->stats_snapshot();
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage& source) {
    auto& context = metal_context();
    if (!context.valid()) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = context.diagnostic(),
        };
    }

    auto preparation = prepare_warm_gpu_resident_resources(source, context.device());
    if (!preparation.resources) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = std::move(preparation.diagnostic),
        };
    }

    auto impl = std::make_unique<WarmEditGpuSession::Impl>();
    impl->resident = std::move(preparation.resources);
    return WarmEditGpuPreparation{
        .session = std::shared_ptr<WarmEditGpuSession>(
            new WarmEditGpuSession(std::move(impl))
        ),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
