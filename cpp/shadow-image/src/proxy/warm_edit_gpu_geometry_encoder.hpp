#pragma once

#include "warm_edit_gpu_geometry_plan.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"

#include <cstdint>

namespace shadow::image::detail {

void encode_warm_gpu_geometry(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    id<MTLBuffer> input,
    id<MTLBuffer> output,
    id<MTLBuffer> status,
    id<MTLBuffer> liquify_words,
    std::uint32_t input_row_floats,
    const WarmGpuGeometryPlan& plan
);

} // namespace shadow::image::detail
