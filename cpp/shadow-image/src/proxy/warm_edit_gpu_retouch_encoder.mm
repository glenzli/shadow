#include "warm_edit_gpu_retouch_encoder.hpp"

#include <algorithm>

namespace shadow::image::detail {

id<MTLBuffer> encode_warm_retouch_clone_stage(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    const WarmRetouchCloneStage& stage,
    id<MTLBuffer> geometry
) {
    if (geometry == nil || !stage.valid()) {
        return nil;
    }
    const auto dispatch = [encoder](
        id<MTLComputePipelineState> pipeline,
        const Dimensions dimensions
    ) {
        const NSUInteger thread_width = std::min<NSUInteger>(
            32U,
            std::max<NSUInteger>(1U, pipeline.threadExecutionWidth)
        );
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(
                8U,
                pipeline.maxTotalThreadsPerThreadgroup / thread_width
            )
        );
        [encoder dispatchThreads:MTLSizeMake(
                dimensions.width,
                dimensions.height,
                1U
            )
            threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
    };

    id<MTLBuffer> current = input;
    for (const WarmRetouchCloneRegion& region : stage.regions) {
        id<MTLBuffer> output = current == slot.adjusted ? slot.denoised : slot.adjusted;
        const WarmRetouchCloneParameters& parameters = region.parameters;
        [encoder setComputePipelineState:context.retouch_clone_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:output offset:0U atIndex:1U];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
        [encoder setBuffer:slot.status offset:0U atIndex:3U];
        [encoder setBuffer:geometry
                    offset:region.capsule_offset_bytes
                   atIndex:4U];
        [encoder setBuffer:geometry
                    offset:region.cell_offset_bytes
                   atIndex:5U];
        [encoder setBuffer:geometry
                    offset:region.reference_offset_bytes
                   atIndex:6U];
        dispatch(context.retouch_clone_pipeline(), layout.dimensions);
        current = output;
    }
    return current;
}

} // namespace shadow::image::detail
