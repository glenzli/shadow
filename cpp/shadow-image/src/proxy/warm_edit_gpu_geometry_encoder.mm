#include "warm_edit_gpu_geometry_encoder.hpp"

#include "warm_edit_gpu_transaction_encoder.hpp"

namespace shadow::image::detail {

void encode_warm_gpu_geometry(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    id<MTLBuffer> input,
    id<MTLBuffer> output,
    id<MTLBuffer> status,
    id<MTLBuffer> liquify_words,
    const std::uint32_t input_row_floats,
    const WarmGpuGeometryPlan& plan
) {
    WarmPhotoGeometryParameters parameters = plan.parameters;
    parameters.input_row_floats = input_row_floats;
    [encoder setComputePipelineState:context.geometry_pipeline()];
    [encoder setBuffer:input offset:0U atIndex:0U];
    [encoder setBuffer:output offset:0U atIndex:1U];
    [encoder setBytes:&parameters
               length:sizeof(parameters)
              atIndex:2U];
    [encoder setBuffer:status offset:0U atIndex:3U];
    [encoder setBytes:&plan.liquify_parameters
               length:sizeof(plan.liquify_parameters)
              atIndex:4U];
    [encoder setBuffer:liquify_words offset:0U atIndex:5U];
    dispatch_warm_gpu_raster(
        encoder,
        context.geometry_pipeline(),
        plan.output_dimensions
    );
}

} // namespace shadow::image::detail
