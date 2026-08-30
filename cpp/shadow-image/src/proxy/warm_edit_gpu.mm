#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_dispatcher.hpp"
#include "warm_edit_gpu_layer_dispatcher.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_presentation_surface.hpp"
#include "warm_edit_gpu_resident_resources.hpp"

#include <shadow/image/warm_edit_preview.hpp>

#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace shadow::image::detail {

struct WarmEditGpuSession::Impl final {
    std::unique_ptr<WarmGpuResidentResources> resident;
    std::mutex host_source_mutex;
    std::shared_ptr<const FloatRgbImage> host_source;
    std::uint64_t host_source_bytes = 0U;
};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) const {
    return render(
        nodes,
        plan,
        retain_linear_for_analysis,
        WarmEditGpuRenderContext{},
        cancellation
    );
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis,
    const WarmEditGpuRenderContext context,
    const std::stop_token cancellation
) const {
    if (cancellation.stop_requested()) {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    }
    if (!impl_ || !impl_->resident) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }
    return dispatch_warm_edit_gpu(
        *impl_->resident,
        nodes,
        plan,
        retain_linear_for_analysis,
        context,
        cancellation
    );
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    const std::span<const AdjustmentLayer> layers,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) const {
    return render_layers(
        layers,
        retain_linear_for_analysis,
        WarmEditGpuRenderContext{},
        std::nullopt,
        cancellation
    );
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    const std::span<const AdjustmentLayer> layers,
    const bool retain_linear_for_analysis,
    const WarmEditGpuRenderContext context,
    const std::stop_token cancellation
) const {
    return render_layers(layers, retain_linear_for_analysis, context, std::nullopt, cancellation);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    const std::span<const AdjustmentLayer> layers,
    const bool retain_linear_for_analysis,
    const WarmEditGpuRenderContext context,
    const std::optional<std::uint32_t> target_layer_index,
    const std::stop_token cancellation
) const {
    if (cancellation.stop_requested()) {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    }
    if (!impl_ || !impl_->resident) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }
    return dispatch_warm_edit_gpu_layers(
        *impl_->resident,
        layers,
        retain_linear_for_analysis,
        context,
        target_layer_index,
        cancellation
    );
}

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    if (!impl_ || !impl_->resident) {
        return {};
    }
    std::lock_guard lock(impl_->host_source_mutex);
    WarmEditPreviewGpuStats stats = impl_->resident->stats_snapshot();
    if (impl_->host_source_bytes
        > std::numeric_limits<std::uint64_t>::max() - stats.resident_bytes) {
        // Never let an impossible accounting state wrap into a small cache charge. A saturated
        // value makes every bounded cache reject this session conservatively.
        stats.resident_bytes = std::numeric_limits<std::uint64_t>::max();
    } else {
        stats.resident_bytes += impl_->host_source_bytes;
    }
    return stats;
}

WarmEditGpuHostSourceAttempt WarmEditGpuSession::host_source_for_cpu_replay(
    const std::stop_token cancellation
) const {
    if (cancellation.stop_requested()) {
        return {.cancelled = true};
    }
    if (!impl_ || !impl_->resident) {
        return {
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }

    std::lock_guard lock(impl_->host_source_mutex);
    if (cancellation.stop_requested()) {
        return {.cancelled = true};
    }
    if (impl_->host_source) {
        return {.source = impl_->host_source};
    }

    const WarmGpuResidentLayout& layout = impl_->resident->layout();
    if (layout.dimensions.height == 0U
        || layout.source_row_stride_bytes > std::numeric_limits<std::size_t>::max()
                                                   / layout.dimensions.height) {
        return {.diagnostic = "resident warm-preview source layout cannot be materialized"};
    }
    const std::size_t source_bytes =
        layout.source_row_stride_bytes * static_cast<std::size_t>(layout.dimensions.height);
    if (source_bytes == 0U || source_bytes % sizeof(float) != 0U) {
        return {.diagnostic = "resident warm-preview source byte layout is invalid"};
    }
    const std::uint64_t host_source_bytes = static_cast<std::uint64_t>(source_bytes);
    const WarmEditPreviewGpuStats resident_stats = impl_->resident->stats_snapshot();
    if (static_cast<std::size_t>(host_source_bytes) != source_bytes
        || host_source_bytes
               > std::numeric_limits<std::uint64_t>::max() - resident_stats.resident_bytes) {
        return {
            .diagnostic =
                "resident warm-preview CPU replay would overflow its retained-byte accounting",
        };
    }

    auto& context = metal_context();
    id<MTLBuffer> source = impl_->resident->source_buffer();
    if (!context.valid() || source == nil || static_cast<std::size_t>(source.length) < source_bytes) {
        return {.diagnostic = "resident warm-preview source is unavailable for CPU replay"};
    }

    auto materialized = std::make_shared<FloatRgbImage>();
    materialized->dimensions = layout.dimensions;
    materialized->row_stride_bytes = layout.source_row_stride_bytes;
    materialized->pixel_format = layout.pixel_format;
    materialized->transfer_function = layout.transfer_function;
    materialized->reference = layout.reference;
    materialized->working_space = layout.working_space;
    materialized->level_zero_to_raster_scale_x = layout.level_zero_to_raster_scale_x;
    materialized->level_zero_to_raster_scale_y = layout.level_zero_to_raster_scale_y;
    materialized->samples.resize(source_bytes / sizeof(float));

    @autoreleasepool {
        id<MTLBuffer> staging = [[context.device()
            newBufferWithLength:source_bytes
                        options:MTLResourceStorageModeShared] autorelease];
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
        id<MTLBlitCommandEncoder> encoder =
            command_buffer == nil ? nil : [command_buffer blitCommandEncoder];
        if (staging == nil || encoder == nil) {
            return {
                .diagnostic = "Metal could not prepare the resident warm-preview CPU replay",
            };
        }
        [encoder copyFromBuffer:source
                   sourceOffset:0U
                       toBuffer:staging
              destinationOffset:0U
                           size:source_bytes];
        [encoder endEncoding];
        [command_buffer commit];
        // Metal cannot cooperatively cancel a committed blit. Cancellation is checked before
        // submission and again before publication; an in-flight request may wait for this one
        // bounded source copy to finish, after which the reusable host source remains cached.
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return {.diagnostic = command_buffer_diagnostic(command_buffer)};
        }
        std::memcpy(materialized->samples.data(), staging.contents, source_bytes);
    }

    impl_->host_source = std::move(materialized);
    impl_->host_source_bytes = host_source_bytes;
    if (cancellation.stop_requested()) {
        return {.cancelled = true};
    }
    return {.source = impl_->host_source};
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(
    const FloatRgbImage& source,
    const SensorClippingMask* sensor_clipping_mask,
    const HighlightChromaRiskMap* highlight_chroma_risk_map
) {
    auto& context = metal_context();
    if (!context.valid()) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = context.diagnostic(),
        };
    }
    // Compile the small presentation-pack pipeline during source preparation. Its failure is
    // deliberately non-fatal: host RGB8 remains the exact fallback, and a later surface request
    // records the cached reason instead of stalling the first slider movement.
    static_cast<void>(prewarm_warm_edit_gpu_presentation_surface(context.device()));

    auto preparation =
        prepare_warm_gpu_resident_resources(
            source, context.device(), sensor_clipping_mask, highlight_chroma_risk_map
        );
    if (!preparation.resources) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = std::move(preparation.diagnostic),
        };
    }

    auto impl = std::make_unique<WarmEditGpuSession::Impl>();
    impl->resident = std::move(preparation.resources);
    return WarmEditGpuPreparation{
        .session = std::shared_ptr<WarmEditGpuSession>(new WarmEditGpuSession(std::move(impl))),
        .diagnostic = {},
    };
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const WarmEditGpuAdoptedSource& source) {
    auto& context = metal_context();
    if (!context.valid()) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = context.diagnostic(),
        };
    }
    if (source.native_device_handle == nullptr || source.native_buffer_handle == nullptr
        || source.source_buffer_bytes == 0U || source.resident_allowance_bytes == 0U
        || source.source_buffer_bytes > source.resident_allowance_bytes) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "adopted warm-edit source has an invalid device resource contract",
        };
    }
    id<MTLDevice> device = (id<MTLDevice>)source.native_device_handle;
    id<MTLBuffer> buffer = (id<MTLBuffer>)source.native_buffer_handle;
    if (device == nil || buffer == nil
        || static_cast<std::uint64_t>(device.registryID)
               != static_cast<std::uint64_t>(context.device().registryID)
        || static_cast<std::uint64_t>(buffer.length) < source.source_buffer_bytes) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic =
                "adopted warm-edit source is not a complete buffer on the active Metal device",
        };
    }

    static_cast<void>(prewarm_warm_edit_gpu_presentation_surface(context.device()));
    FloatRgbImage layout{
        .dimensions = source.dimensions,
        .row_stride_bytes = source.row_stride_bytes,
        .pixel_format = FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = TransferFunction::linear,
        .reference = ImageReference::scene_referred,
        .working_space = source.working_space,
        .level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y,
        .samples = {},
    };
    auto preparation = prepare_warm_gpu_resident_resources(
        layout,
        context.device(),
        buffer,
        source.external_resident_bytes,
        source.resident_allowance_bytes,
        source.sensor_clipping_mask,
        source.highlight_chroma_risk_map
    );
    if (!preparation.resources) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = std::move(preparation.diagnostic),
        };
    }

    auto impl = std::make_unique<WarmEditGpuSession::Impl>();
    impl->resident = std::move(preparation.resources);
    return WarmEditGpuPreparation{
        .session = std::shared_ptr<WarmEditGpuSession>(new WarmEditGpuSession(std::move(impl))),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
