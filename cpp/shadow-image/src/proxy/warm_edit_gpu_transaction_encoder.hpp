#pragma once

#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_resident_resources.hpp"
#include "warm_edit_gpu_transaction.hpp"

namespace shadow::image::detail {

void dispatch_warm_gpu_raster(
    id<MTLComputeCommandEncoder> encoder,
    id<MTLComputePipelineState> pipeline,
    Dimensions dimensions
);

void bind_warm_gpu_adjustment(
    id<MTLComputeCommandEncoder> encoder,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    id<MTLBuffer> output,
    const PreparedWarmProgram& prepared
);

[[nodiscard]] id<MTLBuffer> encode_warm_gpu_transaction_prefix(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    id<MTLBuffer> highlight_clipping,
    const PreparedWarmTransaction& transaction
);

} // namespace shadow::image::detail
