#pragma once

#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_resident_resources.hpp"
#include "warm_edit_gpu_retouch_plan.hpp"

namespace shadow::image::detail {

[[nodiscard]] id<MTLBuffer> encode_warm_retouch_clone_stage(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    const WarmRetouchCloneStage& stage,
    id<MTLBuffer> geometry
);

} // namespace shadow::image::detail
