// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu_layer_dispatcher.hpp"

#include "warm_edit_gpu_geometry_encoder.hpp"
#include "warm_edit_gpu_geometry_plan.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_layer_plan.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_presentation_surface.hpp"
#include "warm_edit_gpu_resident_resources.hpp"
#include "warm_edit_gpu_transaction.hpp"
#include "warm_edit_gpu_transaction_encoder.hpp"

#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

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

struct PreparedWarmLayer final {
    std::size_t plan_index = 0U;
    PreparedWarmTransaction transaction;
    RetainedMetalBuffer brush_index_buffer;
};

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

[[nodiscard]] FloatRgbImage source_layout(const WarmGpuResidentLayout& layout) {
    return FloatRgbImage{
        .dimensions = layout.dimensions,
        .row_stride_bytes = layout.source_row_stride_bytes,
        .pixel_format = layout.pixel_format,
        .transfer_function = layout.transfer_function,
        .reference = layout.reference,
        .working_space = layout.working_space,
        .level_zero_to_raster_scale_x = layout.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = layout.level_zero_to_raster_scale_y,
        .samples = {},
    };
}

} // namespace

WarmEditGpuSession::RenderAttempt dispatch_warm_edit_gpu_layers(
    WarmGpuResidentResources& resident,
    const std::span<const AdjustmentLayer> layers,
    const bool retain_linear_for_analysis,
    const WarmEditGpuRenderContext render_context,
    const std::optional<std::uint32_t> target_layer_index,
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

    const WarmGpuResidentLayout& layout = resident.layout();
    WarmGpuLayerPlan layer_plan = prepare_warm_gpu_layer_plan(
        source_layout(layout),
        layers,
        render_context,
        target_layer_index
    );
    if (!layer_plan.complete) {
        return failed(std::move(layer_plan.diagnostic));
    }

    const std::uint32_t source_row_floats =
        static_cast<std::uint32_t>(layout.source_row_stride_bytes / sizeof(float));
    const std::uint32_t packed_row_floats =
        static_cast<std::uint32_t>(layout.adjusted_row_stride_bytes / sizeof(float));
    std::optional<WarmGpuGeometryPlan> geometry_plan;
    RetainedMetalBuffer geometry_liquify_buffer;
    if (render_context.geometry.has_value()) {
        auto geometry = prepare_warm_gpu_geometry_plan(
            layout.dimensions,
            source_row_floats,
            layout.level_zero_to_raster_scale_x,
            layout.level_zero_to_raster_scale_y,
            *render_context.geometry
        );
        if (!geometry.plan.has_value()) {
            return failed(
                geometry.diagnostic.empty()
                    ? "session-resident Metal layer photo geometry is unavailable"
                    : std::move(geometry.diagnostic)
            );
        }
        geometry_plan = std::move(*geometry.plan);
        auto buffer =
            resident.acquire_liquify_geometry_buffer(geometry_plan->liquify_words, cancellation);
        if (buffer.cancelled) {
            return cancelled();
        }
        if (!buffer.buffer) {
            return failed(
                buffer.diagnostic.empty()
                    ? "session-resident Metal layer render has no photo Liquify side table"
                    : std::move(buffer.diagnostic)
            );
        }
        geometry_liquify_buffer = std::move(buffer.buffer);
    }
    std::size_t operation_count = 0U;
    std::vector<PreparedWarmLayer> prepared_layers;
    prepared_layers.reserve(layer_plan.active_layers.size());
    for (std::size_t index = 0U; index < layer_plan.active_layers.size(); ++index) {
        const WarmGpuLayerPlanEntry& entry = layer_plan.active_layers[index];
        auto preparation = prepare_warm_gpu_transaction(
            resident,
            layers[entry.layer_index].nodes,
            entry.execution,
            render_context,
            index == 0U ? source_row_floats : packed_row_floats,
            operation_count,
            cancellation
        );
        if (preparation.cancelled) {
            return cancelled();
        }
        if (!preparation.transaction.has_value()) {
            return failed(std::move(preparation.diagnostic));
        }
        RetainedMetalBuffer brush_index_buffer;
        if (entry.brush_index.has_value()) {
            auto buffer_attempt =
                resident.acquire_brush_index_buffer(entry.brush_index->words, cancellation);
            if (buffer_attempt.cancelled) {
                return cancelled();
            }
            if (!buffer_attempt.buffer) {
                return failed(
                    buffer_attempt.diagnostic.empty()
                        ? "resident Metal could not upload the brush spatial index"
                        : std::move(buffer_attempt.diagnostic)
                );
            }
            brush_index_buffer = std::move(buffer_attempt.buffer);
        }
        prepared_layers.push_back(
            PreparedWarmLayer{
                .plan_index = index,
                .transaction = std::move(*preparation.transaction),
                .brush_index_buffer = std::move(brush_index_buffer),
            }
        );
    }
    RetainedMetalBuffer mask_coverage_brush_index_buffer;
    if (layer_plan.mask_coverage.has_value()
        && layer_plan.mask_coverage->mask.brush_index.has_value()) {
        auto buffer_attempt = resident.acquire_brush_index_buffer(
            layer_plan.mask_coverage->mask.brush_index->words,
            cancellation
        );
        if (buffer_attempt.cancelled) {
            return cancelled();
        }
        if (!buffer_attempt.buffer) {
            return failed(
                buffer_attempt.diagnostic.empty()
                    ? "resident Metal could not upload mask-coverage brush index"
                    : std::move(buffer_attempt.diagnostic)
            );
        }
        mask_coverage_brush_index_buffer = std::move(buffer_attempt.buffer);
    }

    const EditExecutionPlan empty_plan{};
    auto display_preparation = prepare_warm_gpu_transaction(
        resident,
        {},
        empty_plan,
        render_context,
        prepared_layers.empty() ? source_row_floats : packed_row_floats,
        operation_count,
        cancellation
    );
    if (display_preparation.cancelled) {
        return cancelled();
    }
    if (!display_preparation.transaction.has_value()) {
        return failed(std::move(display_preparation.diagnostic));
    }
    PreparedWarmTransaction display_transaction = std::move(*display_preparation.transaction);

    auto slot_lease = resident.acquire_slot(cancellation);
    if (!slot_lease.has_value()) {
        return cancelled();
    }
    bool needs_layer_snapshot = false;
    for (const PreparedWarmLayer& layer : prepared_layers) {
        const std::string diagnostic =
            ensure_warm_gpu_transaction_resources(*slot_lease, layer.transaction);
        if (!diagnostic.empty()) {
            return failed(diagnostic);
        }
        needs_layer_snapshot =
            needs_layer_snapshot || layer_plan.active_layers[layer.plan_index].needs_blend;
    }
    if (needs_layer_snapshot) {
        const std::string diagnostic = slot_lease->ensure_layer_resources();
        if (!diagnostic.empty()) {
            return failed(diagnostic);
        }
    }
    if (layer_plan.mask_coverage.has_value()) {
        const std::string diagnostic = slot_lease->ensure_mask_coverage_resources();
        if (!diagnostic.empty()) {
            return failed(diagnostic);
        }
    }
    if (retain_linear_for_analysis || geometry_plan.has_value()) {
        // Sequential pixel-local layers can finish in the ordinary adjusted buffer even though
        // no neighborhood stage requested the alternate RGB allocation. The display kernel must
        // preserve that final layer result in a distinct host-readable buffer for settled
        // histogram analysis; binding a missing alternate is legal to Metal but leaves no bytes
        // for the host to read.
        const std::string diagnostic = slot_lease->ensure_denoise_resources();
        if (!diagnostic.empty()) {
            return failed(diagnostic);
        }
    }
    const WarmGpuSlotBuffers slot = slot_lease->buffers();

    @autoreleasepool {
        for (const PreparedWarmLayer& layer : prepared_layers) {
            copy_warm_gpu_transaction_operations(slot.before_operations, layer.transaction);
        }
        copy_warm_gpu_transaction_operations(slot.before_operations, display_transaction);
        auto* status = static_cast<WarmStatus*>([slot.status contents]);
        *status = WarmStatus{};
        const WarmDisplayParameters display{
            .output_origin_x = render_context.display_origin_x,
            .output_origin_y = render_context.display_origin_y,
            .apply_scene_curve = layout.reference == ImageReference::scene_referred ? 1U : 0U,
            .retain_linear = retain_linear_for_analysis ? 1U : 0U,
        };

        WarmMetalContext& context = metal_context();
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return failed("Metal could not create a warm-preview layer command");
        }

        id<MTLBuffer> current = resident.source_buffer();
        const auto encode_mask_coverage = [&](id<MTLBuffer> input) {
            WarmLayerBlendParameters parameters = layer_plan.mask_coverage->mask.parameters;
            parameters.input_row_floats =
                input == resident.source_buffer() ? source_row_floats : packed_row_floats;
            const WarmGpuBrushIndex* brush_index =
                layer_plan.mask_coverage->mask.brush_index.has_value()
                    ? &*layer_plan.mask_coverage->mask.brush_index
                    : nullptr;
            id<MTLBuffer> brush_buffer = mask_coverage_brush_index_buffer
                                             ? mask_coverage_brush_index_buffer.get()
                                             : slot.before_operations;
            [encoder setComputePipelineState:context.mask_coverage_pipeline()];
            [encoder setBuffer:input offset:0U atIndex:0U];
            [encoder setBuffer:slot.mask_coverage_linear offset:0U atIndex:1U];
            [encoder setBuffer:slot.mask_coverage_r8 offset:0U atIndex:2U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3U];
            [encoder setBuffer:slot.status offset:0U atIndex:4U];
            [encoder setBuffer:brush_buffer
                        offset:brush_index == nullptr ? 0U : brush_index->capsule_offset_bytes
                       atIndex:5U];
            [encoder setBuffer:brush_buffer
                        offset:brush_index == nullptr ? 0U : brush_index->cell_range_offset_bytes
                       atIndex:6U];
            [encoder setBuffer:brush_buffer
                        offset:brush_index == nullptr ? 0U : brush_index->reference_offset_bytes
                       atIndex:7U];
            dispatch_warm_gpu_raster(encoder, context.mask_coverage_pipeline(), layout.dimensions);
        };

        std::size_t prepared_layer_index = 0U;
        for (std::size_t authored_layer_index = 0U; authored_layer_index < layers.size();
             ++authored_layer_index) {
            if (layer_plan.mask_coverage.has_value()
                && layer_plan.mask_coverage->layer_index == authored_layer_index) {
                encode_mask_coverage(current);
            }
            if (prepared_layer_index >= prepared_layers.size()) {
                continue;
            }
            const PreparedWarmLayer& layer = prepared_layers[prepared_layer_index];
            WarmGpuLayerPlanEntry& entry = layer_plan.active_layers[layer.plan_index];
            if (entry.layer_index != authored_layer_index) {
                continue;
            }
            ++prepared_layer_index;
            if (entry.needs_blend) {
                entry.blend.input_row_floats =
                    current == resident.source_buffer() ? source_row_floats : packed_row_floats;
                [encoder setComputePipelineState:context.layer_copy_pipeline()];
                [encoder setBuffer:current offset:0U atIndex:0U];
                [encoder setBuffer:slot.layer_before offset:0U atIndex:1U];
                [encoder setBytes:&entry.blend length:sizeof(entry.blend) atIndex:2U];
                dispatch_warm_gpu_raster(encoder, context.layer_copy_pipeline(), layout.dimensions);
            }

            current = encode_warm_gpu_transaction_prefix(
                encoder,
                context,
                layout,
                slot,
                current,
                resident.highlight_clipping_buffer(),
                layer.transaction
            );
            if (!layer.transaction.final_program.program.operations.empty()) {
                id<MTLBuffer> output =
                    current == resident.source_buffer() ? slot.adjusted : current;
                [encoder setComputePipelineState:context.adjustment_pipeline()];
                bind_warm_gpu_adjustment(
                    encoder,
                    slot,
                    current,
                    output,
                    layer.transaction.final_program
                );
                dispatch_warm_gpu_raster(encoder, context.adjustment_pipeline(), layout.dimensions);
                current = output;
            }

            if (entry.needs_blend) {
                const WarmGpuBrushIndex* brush_index =
                    entry.brush_index.has_value() ? &*entry.brush_index : nullptr;
                id<MTLBuffer> brush_buffer = layer.brush_index_buffer
                                                 ? layer.brush_index_buffer.get()
                                                 : slot.before_operations;
                id<MTLBuffer> precomputed_coverage = entry.blend.use_precomputed_coverage != 0U
                                                         ? slot.mask_coverage_linear
                                                         : slot.before_operations;
                [encoder setComputePipelineState:context.layer_blend_pipeline()];
                [encoder setBuffer:slot.layer_before offset:0U atIndex:0U];
                [encoder setBuffer:current offset:0U atIndex:1U];
                [encoder setBytes:&entry.blend length:sizeof(entry.blend) atIndex:2U];
                [encoder setBuffer:slot.status offset:0U atIndex:3U];
                [encoder setBuffer:brush_buffer
                            offset:brush_index == nullptr ? 0U : brush_index->capsule_offset_bytes
                           atIndex:4U];
                [encoder
                    setBuffer:brush_buffer
                       offset:brush_index == nullptr ? 0U : brush_index->cell_range_offset_bytes
                      atIndex:5U];
                [encoder setBuffer:brush_buffer
                            offset:brush_index == nullptr ? 0U : brush_index->reference_offset_bytes
                           atIndex:6U];
                [encoder setBuffer:precomputed_coverage offset:0U atIndex:7U];
                dispatch_warm_gpu_raster(
                    encoder,
                    context.layer_blend_pipeline(),
                    layout.dimensions
                );
            }
        }

        const PreparedWarmProgram& display_program = display_transaction.final_program;
        Dimensions output_dimensions = layout.dimensions;
        std::size_t output_sample_count = layout.adjusted_sample_count;
        std::size_t output_linear_bytes = layout.adjusted_bytes;
        std::size_t output_rgb8_bytes = layout.rgb8_bytes;
        std::size_t output_mask_coverage_bytes =
            static_cast<std::size_t>(layout.dimensions.pixel_count());
        double output_scale_x = layout.level_zero_to_raster_scale_x;
        double output_scale_y = layout.level_zero_to_raster_scale_y;
        auto display_invocation = display_program.program.invocation;
        if (geometry_plan.has_value()) {
            id<MTLBuffer> geometry_output =
                current == slot.adjusted ? slot.denoised : slot.adjusted;
            const std::uint32_t geometry_input_row_floats =
                current == resident.source_buffer() ? source_row_floats : packed_row_floats;
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
            output_sample_count = static_cast<std::size_t>(output_dimensions.pixel_count()) * 3U;
            output_linear_bytes = output_sample_count * sizeof(float);
            output_rgb8_bytes = output_sample_count;
            output_scale_x = geometry_plan->output_level_zero_to_raster_scale_x;
            output_scale_y = geometry_plan->output_level_zero_to_raster_scale_y;
            display_invocation.width = output_dimensions.width;
            display_invocation.height = output_dimensions.height;
            display_invocation.input_row_floats = output_dimensions.width * 3U;
            display_invocation.output_row_floats = output_dimensions.width * 3U;
            output_mask_coverage_bytes = static_cast<std::size_t>(output_dimensions.pixel_count());
            if (layer_plan.mask_coverage.has_value()) {
                const WarmPhotoGeometryParameters mask_geometry = geometry_plan->parameters;
                [encoder setComputePipelineState:context.mask_coverage_geometry_pipeline()];
                [encoder setBuffer:slot.mask_coverage_linear offset:0U atIndex:0U];
                [encoder setBuffer:slot.mask_coverage_r8 offset:0U atIndex:1U];
                [encoder setBytes:&mask_geometry length:sizeof(mask_geometry) atIndex:2U];
                [encoder setBuffer:slot.status offset:0U atIndex:3U];
                [encoder setBytes:&geometry_plan->liquify_parameters
                           length:sizeof(geometry_plan->liquify_parameters)
                          atIndex:4U];
                [encoder setBuffer:geometry_liquify_buffer.get() offset:0U atIndex:5U];
                dispatch_warm_gpu_raster(
                    encoder,
                    context.mask_coverage_geometry_pipeline(),
                    output_dimensions
                );
            }
        }
        // Interactive display never writes linear output, but Metal still requires a valid
        // binding. Reuse the ordinary buffer instead of allocating an unused alternate.
        id<MTLBuffer> final_adjusted =
            retain_linear_for_analysis && current == slot.adjusted ? slot.denoised : slot.adjusted;
        [encoder setComputePipelineState:context.display_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:final_adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:slot.before_operations
                    offset:display_program.operation_offset_bytes
                   atIndex:3U];
        [encoder setBytes:&display_invocation length:sizeof(display_invocation) atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];
        [encoder setBuffer:display_program.buffers.curve.get() offset:0U atIndex:7U];
        [encoder setBuffer:display_program.buffers.lut.get() offset:0U atIndex:8U];
        [encoder setBuffer:display_program.buffers.perceptual_mixer.get() offset:0U atIndex:9U];
        [encoder setBuffer:display_program.buffers.perceptual_range.get() offset:0U atIndex:10U];
        [encoder setBuffer:display_program.buffers.selective_color.get() offset:0U atIndex:11U];
        [encoder setBuffer:display_program.buffers.paint.get() offset:0U atIndex:12U];
        dispatch_warm_gpu_raster(encoder, context.display_pipeline(), output_dimensions);
        [encoder endEncoding];

        std::shared_ptr<WarmEditGpuPresentationSurface> presentation_surface;
        std::string presentation_fallback_diagnostic;
        if (render_context.output_intent == WarmEditGpuOutputIntent::metal_presentation_surface
            && !retain_linear_for_analysis) {
            resident.record_presentation_surface_request();
            auto preparation_surface =
                prepare_warm_edit_gpu_presentation_surface(context.device(), output_dimensions);
            if (preparation_surface.surface) {
                const std::string presentation_diagnostic =
                    encode_warm_edit_gpu_presentation_surface(
                        command_buffer,
                        slot.rgb8,
                        *preparation_surface.surface
                    );
                if (presentation_diagnostic.empty()) {
                    presentation_surface = std::move(preparation_surface.surface);
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
            return failed(
                "session-resident Metal layer blend produced an invalid result"
                " (flags="
                + std::to_string(status->flags)
                + ", earliest-step=" + std::to_string(status->earliest_step) + ")"
            );
        }
        if (presentation_surface) {
            resident.record_presentation_surface_publish();
        } else if (!presentation_fallback_diagnostic.empty()) {
            resident.record_presentation_surface_fallback();
        }

        RenderResult result{
            .dimensions = output_dimensions,
            .rgb8 = presentation_surface ? std::vector<std::uint8_t>{}
                                         : std::vector<std::uint8_t>(output_rgb8_bytes),
            .presentation_surface = std::move(presentation_surface),
            .presentation_fallback_diagnostic = std::move(presentation_fallback_diagnostic),
            .analyzed_linear = std::nullopt,
            .mask_coverage = std::nullopt,
            .had_active_adjustments = !prepared_layers.empty() || geometry_plan.has_value(),
        };
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        if (!result.presentation_surface) {
            std::memcpy(result.rgb8.data(), [slot.rgb8 contents], output_rgb8_bytes);
        }
        if (layer_plan.mask_coverage.has_value()) {
            WarmEditGpuSession::MaskCoverageResult mask{
                .layer_index = layer_plan.mask_coverage->layer_index,
                .dimensions = output_dimensions,
                .row_stride_bytes = output_dimensions.width,
                .samples = std::vector<std::uint8_t>(output_mask_coverage_bytes),
            };
            std::memcpy(
                mask.samples.data(),
                [slot.mask_coverage_r8 contents],
                output_mask_coverage_bytes
            );
            result.mask_coverage = std::move(mask);
        }
        if (retain_linear_for_analysis) {
            FloatRgbImage linear{
                .dimensions = output_dimensions,
                .row_stride_bytes =
                    static_cast<std::size_t>(output_dimensions.width) * 3U * sizeof(float),
                .pixel_format = layout.pixel_format,
                .transfer_function = layout.transfer_function,
                .reference = layout.reference,
                .working_space = layout.working_space,
                .level_zero_to_raster_scale_x = output_scale_x,
                .level_zero_to_raster_scale_y = output_scale_y,
                .samples = std::vector<float>(output_sample_count),
            };
            std::memcpy(linear.samples.data(), [final_adjusted contents], output_linear_bytes);
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
