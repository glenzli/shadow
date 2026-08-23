#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_dispatcher.hpp"
#include "warm_edit_gpu_layer_dispatcher.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_presentation_surface.hpp"
#include "warm_edit_gpu_resident_resources.hpp"

#include <shadow/image/warm_edit_preview.hpp>

#include <memory>
#include <utility>

namespace shadow::image::detail {

struct WarmEditGpuSession::Impl final {
    std::unique_ptr<WarmGpuResidentResources> resident;
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
    return impl_->resident->stats_snapshot();
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
