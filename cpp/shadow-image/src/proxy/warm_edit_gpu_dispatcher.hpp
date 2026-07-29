#pragma once

#include "warm_edit_gpu.hpp"

namespace shadow::image::detail {

class WarmGpuResidentResources;

// Execute one already-planned warm preview against a prepared resident resource aggregate.
// Session availability and construction remain the facade's responsibility.
[[nodiscard]] WarmEditGpuSession::RenderAttempt dispatch_warm_edit_gpu(
    WarmGpuResidentResources& resident,
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    bool retain_linear_for_analysis,
    WarmEditGpuRenderContext context,
    std::stop_token cancellation
);

} // namespace shadow::image::detail
