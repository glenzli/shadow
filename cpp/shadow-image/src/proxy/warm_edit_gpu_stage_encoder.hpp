#pragma once

#include "warm_edit_gpu_neighbourhood_plan.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_resident_resources.hpp"

#include "../edit/metal_adjustment_program.hpp"

#include <string>

namespace shadow::image::detail {

[[nodiscard]] std::string
ensure_warm_gpu_stage_resources(WarmGpuSlotLease& slot, const WarmGpuNeighbourhoodStage& stage);

[[nodiscard]] id<MTLBuffer> encode_warm_gpu_neighbourhood_stage(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    const WarmGpuNeighbourhoodStage& stage,
    const MetalAdjustmentInvocation& color_invocation
);

} // namespace shadow::image::detail
