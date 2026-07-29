#include "warm_edit_gpu_retouch_encoder.hpp"

#include <algorithm>
#include <array>

namespace shadow::image::detail {

id<MTLBuffer> encode_warm_retouch_stage(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input,
    const WarmRetouchStage& stage,
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
    const auto dispatch_statistics = [encoder](const std::uint32_t group_count) {
        // Statistics reduction requires a complete power-of-two lane set. dispatchThreadgroups
        // deliberately launches the padded final group; the shader zeroes lanes beyond the
        // region instead of reducing an implementation-defined non-uniform tail.
        [encoder dispatchThreadgroups:MTLSizeMake(group_count, 1U, 1U)
            threadsPerThreadgroup:MTLSizeMake(256U, 1U, 1U)];
    };
    const auto available_buffers = [&slot](id<MTLBuffer> current) {
        std::array<id<MTLBuffer>, 2U> result{nil, nil};
        std::size_t index = 0U;
        for (id<MTLBuffer> candidate :
             std::array<id<MTLBuffer>, 3U>{
                 slot.adjusted,
                 slot.denoised,
                 slot.layer_before,
             }) {
            if (candidate != current && index < result.size()) {
                result[index++] = candidate;
            }
        }
        return result;
    };

    id<MTLBuffer> current = input;
    for (const WarmRetouchRegion& region : stage.regions) {
        WarmRetouchRegionParameters parameters = region.parameters;
        const auto scratch = available_buffers(current);
        if (scratch[0] == nil || scratch[1] == nil) {
            return nil;
        }
        if (parameters.mode == WarmRetouchMode::clone) {
            [encoder setComputePipelineState:context.retouch_clone_pipeline()];
            [encoder setBuffer:current offset:0U atIndex:0U];
            [encoder setBuffer:scratch[0] offset:0U atIndex:1U];
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
            current = scratch[0];
            continue;
        }

        const auto encode_statistics = [&](const std::uint32_t robust_pass) {
            parameters.robust_pass = robust_pass;
            [encoder setComputePipelineState:context.retouch_heal_statistics_pipeline()];
            [encoder setBuffer:current offset:0U atIndex:0U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:1U];
            [encoder setBuffer:slot.status offset:0U atIndex:2U];
            [encoder setBuffer:geometry
                        offset:region.capsule_offset_bytes
                       atIndex:3U];
            [encoder setBuffer:geometry
                        offset:region.cell_offset_bytes
                       atIndex:4U];
            [encoder setBuffer:geometry
                        offset:region.reference_offset_bytes
                       atIndex:5U];
            [encoder setBuffer:slot.retouch_statistics offset:0U atIndex:6U];
            [encoder setBuffer:slot.retouch_summary offset:0U atIndex:7U];
            dispatch_statistics(parameters.statistics_group_count);

            [encoder setComputePipelineState:context.retouch_heal_reduce_pipeline()];
            [encoder setBuffer:slot.retouch_statistics offset:0U atIndex:0U];
            [encoder setBuffer:slot.retouch_summary offset:0U atIndex:1U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
            [encoder dispatchThreads:MTLSizeMake(1U, 1U, 1U)
                threadsPerThreadgroup:MTLSizeMake(1U, 1U, 1U)];
        };
        encode_statistics(0U);
        encode_statistics(1U);

        [encoder setComputePipelineState:context.retouch_heal_initialize_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:scratch[0] offset:0U atIndex:1U];
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
        [encoder setBuffer:slot.retouch_summary offset:0U atIndex:7U];
        dispatch(context.retouch_heal_initialize_pipeline(), layout.dimensions);

        id<MTLBuffer> solution = scratch[0];
        id<MTLBuffer> next = scratch[1];
        for (std::uint32_t iteration = 0U;
             iteration < parameters.poisson_iterations;
             ++iteration) {
            [encoder setComputePipelineState:context.retouch_heal_jacobi_pipeline()];
            [encoder setBuffer:current offset:0U atIndex:0U];
            [encoder setBuffer:solution offset:0U atIndex:1U];
            [encoder setBuffer:next offset:0U atIndex:2U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3U];
            [encoder setBuffer:slot.status offset:0U atIndex:4U];
            [encoder setBuffer:geometry
                        offset:region.capsule_offset_bytes
                       atIndex:5U];
            [encoder setBuffer:geometry
                        offset:region.cell_offset_bytes
                       atIndex:6U];
            [encoder setBuffer:geometry
                        offset:region.reference_offset_bytes
                       atIndex:7U];
            [encoder setBuffer:slot.retouch_summary offset:0U atIndex:8U];
            dispatch(context.retouch_heal_jacobi_pipeline(), layout.dimensions);
            std::swap(solution, next);
        }

        [encoder setComputePipelineState:context.retouch_heal_blend_pipeline()];
        [encoder setBuffer:current offset:0U atIndex:0U];
        [encoder setBuffer:solution offset:0U atIndex:1U];
        [encoder setBuffer:next offset:0U atIndex:2U];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3U];
        [encoder setBuffer:slot.status offset:0U atIndex:4U];
        [encoder setBuffer:geometry
                    offset:region.capsule_offset_bytes
                   atIndex:5U];
        [encoder setBuffer:geometry
                    offset:region.cell_offset_bytes
                   atIndex:6U];
        [encoder setBuffer:geometry
                    offset:region.reference_offset_bytes
                   atIndex:7U];
        [encoder setBuffer:slot.retouch_summary offset:0U atIndex:8U];
        dispatch(context.retouch_heal_blend_pipeline(), layout.dimensions);
        current = next;
    }
    return current;
}

} // namespace shadow::image::detail
