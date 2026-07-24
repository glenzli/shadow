#include "warm_edit_gpu.hpp"

#include <utility>

namespace shadow::image::detail {

struct WarmEditGpuSession::Impl final {};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    std::span<const AdjustmentNode>,
    const EditExecutionPlan&,
    bool,
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

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    return {};
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage&) {
    return WarmEditGpuPreparation{
        .session = nullptr,
        .diagnostic = "session-resident Metal warm preview is unavailable on this platform",
    };
}

} // namespace shadow::image::detail
