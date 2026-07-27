#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_dispatcher.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_resident_resources.hpp"

#include <shadow/image/warm_edit_preview.hpp>

#include <memory>
#include <utility>

namespace shadow::image::detail {

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
        cancellation
    );
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
