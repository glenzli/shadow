#include "warm_edit_gpu.hpp"

#include <shadow/image/warm_edit_preview.hpp>

#include <utility>

namespace shadow::image::detail {

struct WarmEditGpuSession::Impl final {};

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
    std::span<const AdjustmentNode>,
    const EditExecutionPlan&,
    bool,
    WarmEditGpuRenderContext,
    std::stop_token cancellation
) const {
    if (cancellation.stop_requested()) {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    }
    return RenderAttempt{
        .status = RenderStatus::unavailable_or_failed,
        .output = std::nullopt,
        .diagnostic = "session-resident Metal warm preview is unavailable on this platform",
    };
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    std::span<const AdjustmentLayer>,
    const bool,
    const std::stop_token cancellation
) const {
    return render_layers({}, false, WarmEditGpuRenderContext{}, std::nullopt, cancellation);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    std::span<const AdjustmentLayer>,
    const bool,
    const WarmEditGpuRenderContext,
    const std::stop_token cancellation
) const {
    return render_layers({}, false, WarmEditGpuRenderContext{}, std::nullopt, cancellation);
}

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render_layers(
    std::span<const AdjustmentLayer>,
    const bool,
    const WarmEditGpuRenderContext,
    const std::optional<std::uint32_t>,
    const std::stop_token cancellation
) const {
    return RenderAttempt{
        .status = cancellation.stop_requested() ? RenderStatus::cancelled
                                                : RenderStatus::unavailable_or_failed,
        .output = std::nullopt,
        .diagnostic = cancellation.stop_requested()
                          ? std::string{}
                          : "session-resident Metal warm preview is unavailable on this platform",
    };
}

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    return {};
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage&) {
    return WarmEditGpuPreparation{
        .session = nullptr,
        .diagnostic = "session-resident Metal warm preview is unavailable on this platform",
    };
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const WarmEditGpuAdoptedSource&) {
    return WarmEditGpuPreparation{
        .session = nullptr,
        .diagnostic = "adopted Metal warm-edit source is unavailable on this platform",
    };
}

} // namespace shadow::image::detail
