#include "warm_edit_gpu_geometry_encoder.hpp"

#include "warm_edit_gpu_transaction_encoder.hpp"

namespace shadow::image::detail {

void encode_warm_gpu_geometry(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    id<MTLBuffer> input,
    id<MTLBuffer> output,
    id<MTLBuffer> status,
    const WarmGpuGeometryPlan& plan
) {
    [encoder setComputePipelineState:context.geometry_pipeline()];
    [encoder setBuffer:input offset:0U atIndex:0U];
    [encoder setBuffer:output offset:0U atIndex:1U];
    [encoder setBytes:&plan.parameters
               length:sizeof(plan.parameters)
              atIndex:2U];
    [encoder setBuffer:status offset:0U atIndex:3U];
    dispatch_warm_gpu_raster(
        encoder,
        context.geometry_pipeline(),
        plan.output_dimensions
    );
}

} // namespace shadow::image::detail
