#include "../../edit/local_mask_validation.hpp"
#include "../../proxy/warm_edit_gpu.hpp"
#include "../image_acceleration_policy.hpp"
#include "cuda_image_session.hpp"

#include <shadow/image/edit_error.hpp>
#include <utility>

namespace shadow::image::detail {
namespace {
using Attempt = WarmEditGpuSession::RenderAttempt;
using Status = WarmEditGpuSession::RenderStatus;

Attempt declined(std::string reason, std::stop_token cancellation) {
    return {
        .status = cancellation.stop_requested() ? Status::cancelled : Status::unavailable_or_failed,
        .diagnostic = cancellation.stop_requested() ? std::string{} : std::move(reason)
    };
}

std::optional<CudaAdjustmentStage> prepare_stage(
    const FloatRgbImage& layout,
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    AdjustmentExecutionContext context,
    double opacity
) {
    auto prepared = prepare_metal_adjustment(layout, nodes, plan, context, true);
    if (!prepared.program)
        return std::nullopt;
    return CudaAdjustmentStage{std::move(*prepared.program), static_cast<float>(opacity)};
}

Attempt render_stages(
    CudaImageSession& session,
    std::span<const CudaAdjustmentStage> stages,
    bool retain_linear,
    WarmEditGpuRenderContext context,
    std::stop_token cancellation
) {
    if (context.geometry)
        return declined("CUDA geometry requires complete CPU replay", cancellation);
    if (image_acceleration_preference_from_environment() == ImageAccelerationPreference::metal) {
        return declined("Forced Metal execution cannot use the CUDA backend", cancellation);
    }
    try {
        auto output = session.render(
            stages,
            retain_linear,
            context.display_origin_x,
            context.display_origin_y,
            cancellation
        );
        if (!output)
            return {.status = Status::cancelled};
        WarmEditGpuSession::RenderResult result{
            .dimensions = session.layout().dimensions,
            .rgb8 = std::move(output->rgb8),
            .analyzed_linear = std::move(output->linear),
            .had_active_adjustments = !stages.empty(),
            .backend = WarmEditGpuBackend::cuda,
        };
        // Windows' portable Qt consumer explicitly consumes host RGB8. No Metal surface is
        // advertised and no unnecessary readback/upload is hidden inside the device transaction.
        return {.status = Status::completed, .output = std::move(result)};
    } catch (const std::exception& error) {
        return declined(error.what(), cancellation);
    }
}
} // namespace

struct WarmEditGpuSession::Impl final {
    CudaImageSession session;
    explicit Impl(const FloatRgbImage& source) : session(source) {}
};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    bool retain,
    std::stop_token stop
) const {
    return render(nodes, plan, retain, {}, stop);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    bool retain,
    WarmEditGpuRenderContext context,
    std::stop_token stop
) const {
    if (stop.stop_requested())
        return {.status = RenderStatus::cancelled};
    std::vector<CudaAdjustmentStage> stages;
    if (!plan.segments.empty()) {
        auto stage = prepare_stage(impl_->session.layout(), nodes, plan, context.adjustment, 1.0);
        if (!stage)
            return declined("CUDA adjustment requires complete CPU replay for this plan", stop);
        stages.push_back(std::move(*stage));
    }
    return render_stages(impl_->session, stages, retain, context, stop);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    std::span<const AdjustmentLayer> layers,
    bool retain,
    std::stop_token stop
) const {
    return render_layers(layers, retain, {}, std::nullopt, stop);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    std::span<const AdjustmentLayer> layers,
    bool retain,
    WarmEditGpuRenderContext context,
    std::stop_token stop
) const {
    return render_layers(layers, retain, context, std::nullopt, stop);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    std::span<const AdjustmentLayer> layers,
    bool retain,
    WarmEditGpuRenderContext context,
    std::optional<std::uint32_t> target,
    std::stop_token stop
) const {
    if (stop.stop_requested())
        return {.status = RenderStatus::cancelled};
    static_cast<void>(
        validate_adjustment_layer_plan(impl_->session.layout(), layers, context.adjustment)
    );
    if (target)
        return declined("CUDA mask coverage requires complete CPU replay", stop);
    std::vector<CudaAdjustmentStage> stages;
    for (const auto& layer : layers) {
        if (!layer.enabled || layer.opacity == 0.0)
            continue;
        const auto plan = compile_edit_execution_plan(
            layer.nodes,
            impl_->session.layout().level_zero_to_raster_scale_x,
            impl_->session.layout().level_zero_to_raster_scale_y
        );
        if (plan.segments.empty())
            continue;
        if (layer.mask)
            return declined("CUDA local masks require complete CPU replay", stop);
        auto stage = prepare_stage(
            impl_->session.layout(),
            layer.nodes,
            plan,
            context.adjustment,
            layer.opacity
        );
        if (!stage)
            return declined("CUDA layer plan requires complete CPU replay", stop);
        stages.push_back(std::move(*stage));
    }
    return render_stages(impl_->session, stages, retain, context, stop);
}

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    return impl_->session.stats();
}
WarmEditGpuHostSourceAttempt WarmEditGpuSession::host_source_for_cpu_replay(std::stop_token stop
) const {
    if (stop.stop_requested())
        return {.cancelled = true};
    try {
        auto source = impl_->session.host_source();
        if (stop.stop_requested())
            return {.cancelled = true};
        return {.source = std::move(source)};
    } catch (const std::exception& error) {
        return {.diagnostic = error.what()};
    }
}

WarmEditGpuPreparation
prepare_warm_edit_gpu_session(const FloatRgbImage& source, const SensorClippingMask*, const HighlightChromaRiskMap*) {
    if (image_acceleration_preference_from_environment() == ImageAccelerationPreference::cpu) {
        return {.diagnostic = "CUDA preparation disabled by the CPU execution policy"};
    }
    if (image_acceleration_preference_from_environment() == ImageAccelerationPreference::metal) {
        return {.diagnostic = "Forced Metal execution cannot prepare a CUDA source"};
    }
    try {
        auto impl = std::make_unique<WarmEditGpuSession::Impl>(source);
        return {
            .session = std::shared_ptr<WarmEditGpuSession>(new WarmEditGpuSession(std::move(impl)))
        };
    } catch (const std::exception& error) {
        return {.diagnostic = error.what()};
    }
}
WarmEditGpuPreparation prepare_warm_edit_gpu_session(const WarmEditGpuAdoptedSource&) {
    return {.diagnostic = "A Metal device source cannot be adopted by CUDA"};
}
} // namespace shadow::image::detail
