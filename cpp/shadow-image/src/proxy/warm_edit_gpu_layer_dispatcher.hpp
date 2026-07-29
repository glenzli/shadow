#pragma once

#include "warm_edit_gpu.hpp"

namespace shadow::image::detail {

class WarmGpuResidentResources;

[[nodiscard]] WarmEditGpuSession::RenderAttempt dispatch_warm_edit_gpu_layers(
    WarmGpuResidentResources& resident,
    std::span<const AdjustmentLayer> layers,
    bool retain_linear_for_analysis,
    WarmEditGpuRenderContext context,
    std::optional<std::uint32_t> target_layer_index,
    std::stop_token cancellation
);

} // namespace shadow::image::detail
