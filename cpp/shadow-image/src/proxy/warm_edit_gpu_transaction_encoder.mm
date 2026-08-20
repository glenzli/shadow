#include "warm_edit_gpu_transaction_encoder.hpp"

#include "warm_edit_gpu_stage_encoder.hpp"

#include <algorithm>

namespace shadow::image::detail {

void dispatch_warm_gpu_raster(
    id<MTLComputeCommandEncoder> encoder,
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
    [encoder dispatchThreads:MTLSizeMake(dimensions.width, dimensions.height, 1U)
        threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
}

void bind_warm_gpu_adjustment(
    id<MTLComputeCommandEncoder> encoder,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    id<MTLBuffer> output,
    const PreparedWarmProgram& prepared
) {
    [encoder setBuffer:input offset:0U atIndex:0U];
    [encoder setBuffer:output offset:0U atIndex:1U];
    [encoder setBuffer:slot.before_operations
                offset:prepared.operation_offset_bytes
               atIndex:3U];
    [encoder setBytes:&prepared.program.invocation
               length:sizeof(prepared.program.invocation)
              atIndex:4U];
    [encoder setBuffer:slot.status offset:0U atIndex:6U];
    [encoder setBuffer:prepared.buffers.curve.get() offset:0U atIndex:7U];
    [encoder setBuffer:prepared.buffers.lut.get() offset:0U atIndex:8U];
    [encoder setBuffer:prepared.buffers.perceptual_mixer.get() offset:0U atIndex:9U];
    [encoder setBuffer:prepared.buffers.perceptual_range.get() offset:0U atIndex:10U];
    [encoder setBuffer:prepared.buffers.selective_color.get() offset:0U atIndex:11U];
}

id<MTLBuffer> encode_warm_gpu_transaction_prefix(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> source_highlight_chroma_confidence,
    id<MTLBuffer> input,
    const PreparedWarmTransaction& transaction
) {
    id<MTLBuffer> current = input;
    for (const PreparedWarmPass& pass : transaction.passes) {
        const WarmGpuRenderPass& render_pass =
            transaction.render_plan.passes[pass.plan_index];
        [encoder setComputePipelineState:context.adjustment_pipeline()];
        // Pixel-local kernels load one pixel completely before writing it. In-place execution is
        // safe and avoids a redundant full-raster copy between neighborhood stages.
        bind_warm_gpu_adjustment(encoder, slot, current, slot.adjusted, pass.before);
        dispatch_warm_gpu_raster(encoder, context.adjustment_pipeline(), layout.dimensions);
        current = encode_warm_gpu_neighbourhood_stage(
            encoder,
            context,
            layout,
            slot,
            source_highlight_chroma_confidence,
            slot.adjusted,
            render_pass.neighbourhood,
            pass.neighbourhood_geometry.get(),
            pass.post.has_value()
                ? pass.post->program.invocation
                : pass.before.program.invocation
        );
        if (pass.post.has_value()) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_warm_gpu_adjustment(encoder, slot, current, current, *pass.post);
            dispatch_warm_gpu_raster(encoder, context.adjustment_pipeline(), layout.dimensions);
        }
    }
    return current;
}

} // namespace shadow::image::detail
