#include "warm_edit_gpu_stage_encoder.hpp"

#include "warm_edit_gpu_retouch_encoder.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace shadow::image::detail {

namespace {

[[nodiscard]] id<MTLBuffer> alternate_rgb_buffer(
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> input
) noexcept {
    return input == slot.adjusted ? slot.denoised : slot.adjusted;
}

} // namespace

std::string ensure_warm_gpu_stage_resources(
    WarmGpuSlotLease& slot,
    const WarmGpuNeighbourhoodStage& stage
) {
    return std::visit(
        [&slot](const auto& value) {
            using Stage = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Stage, WarmRetouchStage>) {
                return slot.ensure_retouch_resources();
            } else if constexpr (std::is_same_v<Stage, WarmTechnicalDetailStage>) {
                return value.sharpen.has_value()
                    ? slot.ensure_sharpen_resources()
                    : slot.ensure_denoise_resources();
            } else if constexpr (std::is_same_v<Stage, WarmTextureClarityStage>) {
                return slot.ensure_texture_clarity_resources();
            } else if constexpr (
                std::is_same_v<Stage, WarmLocalContrastStage>
                || std::is_same_v<Stage, WarmSelectiveToneStage>
            ) {
                return slot.ensure_local_contrast_resources();
            } else if constexpr (std::is_same_v<Stage, WarmTextureStage>) {
                return slot.ensure_sharpen_resources();
            } else if constexpr (std::is_same_v<Stage, WarmClarityStage>) {
                return slot.ensure_clarity_resources();
            } else {
                return slot.ensure_denoise_resources();
            }
        },
        stage
    );
}

id<MTLBuffer> encode_warm_gpu_neighbourhood_stage(
    id<MTLComputeCommandEncoder> encoder,
    WarmMetalContext& context,
    const WarmGpuResidentLayout& layout,
    const WarmGpuSlotBuffers& slot,
    id<MTLBuffer> source_highlight_chroma_confidence,
    id<MTLBuffer> input,
    const WarmGpuNeighbourhoodStage& stage,
    id<MTLBuffer> neighbourhood_geometry,
    const MetalAdjustmentInvocation& color_invocation
) {
    const auto dispatch_grid = [encoder](
        id<MTLComputePipelineState> pipeline,
        const NSUInteger width,
        const NSUInteger height
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
                width,
                height,
                1U
            )
            threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
    };
    const auto dispatch = [&dispatch_grid, &layout](id<MTLComputePipelineState> pipeline) {
        dispatch_grid(pipeline, layout.dimensions.width, layout.dimensions.height);
    };

    return std::visit(
        [&](const auto& value) {
            using Stage = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Stage, WarmRetouchStage>) {
                return encode_warm_retouch_stage(
                    encoder,
                    context,
                    layout,
                    slot,
                    input,
                    value,
                    neighbourhood_geometry
                );
            } else if constexpr (std::is_same_v<Stage, WarmTechnicalDetailStage>) {
                id<MTLBuffer> output = input;
                if (value.denoise.has_value()) {
                    const auto& denoise = *value.denoise;
                    for (std::uint32_t pass = 0U; pass < denoise.passes; ++pass) {
                        id<MTLBuffer> pass_output = alternate_rgb_buffer(slot, output);
                        [encoder setComputePipelineState:context.denoise_pipeline()];
                        [encoder setBuffer:output offset:0U atIndex:0U];
                        [encoder setBuffer:pass_output offset:0U atIndex:1U];
                        [encoder setBytes:&denoise length:sizeof(denoise) atIndex:2U];
                        dispatch(context.denoise_pipeline());
                        output = pass_output;
                    }
                }
                if (value.dehaze_defringe.has_value()) {
                    const auto& technical_optics = *value.dehaze_defringe;
                    id<MTLBuffer> dehazed = alternate_rgb_buffer(slot, output);
                    [encoder setComputePipelineState:context.dehaze_defringe_pipeline()];
                    [encoder setBuffer:output offset:0U atIndex:0U];
                    [encoder setBuffer:dehazed offset:0U atIndex:1U];
                    [encoder setBytes:&technical_optics
                               length:sizeof(technical_optics)
                              atIndex:2U];
                    [encoder setBytes:&color_invocation
                               length:sizeof(color_invocation)
                              atIndex:3U];
                    dispatch(context.dehaze_defringe_pipeline());
                    output = dehazed;
                }
                if (value.sharpen.has_value()) {
                    const auto& sharpen = *value.sharpen;
                    [encoder setComputePipelineState:context.sharpen_log_pipeline()];
                    [encoder setBuffer:output offset:0U atIndex:0U];
                    [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
                    [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:2U];
                    dispatch(context.sharpen_log_pipeline());

                    [encoder setComputePipelineState:context.sharpen_horizontal_pipeline()];
                    [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                    [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                    [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:2U];
                    dispatch(context.sharpen_horizontal_pipeline());

                    id<MTLBuffer> sharpened = alternate_rgb_buffer(slot, output);
                    [encoder setComputePipelineState:context.sharpen_apply_pipeline()];
                    [encoder setBuffer:output offset:0U atIndex:0U];
                    [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                    [encoder setBuffer:sharpened offset:0U atIndex:2U];
                    [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:3U];
                    dispatch(context.sharpen_apply_pipeline());
                    output = sharpened;
                }
                return output;
            } else if constexpr (std::is_same_v<Stage, WarmTextureClarityStage>) {
                const auto& texture = value.texture_gaussian;
                const auto& small = value.clarity_small_gaussian;
                const auto& large = value.clarity_large_gaussian;
                const auto& combined = value.parameters;
                [encoder setComputePipelineState:context.texture_lightness_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
                [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:3U];
                dispatch(context.texture_lightness_pipeline());

                [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                dispatch(context.texture_horizontal_pipeline());
                [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
                [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:1U];
                [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                dispatch(context.scalar_vertical_pipeline());

                [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&small length:sizeof(small) atIndex:2U];
                dispatch(context.texture_horizontal_pipeline());
                [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
                [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
                [encoder setBytes:&small length:sizeof(small) atIndex:2U];
                dispatch(context.scalar_vertical_pipeline());

                [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&large length:sizeof(large) atIndex:2U];
                dispatch(context.texture_horizontal_pipeline());

                id<MTLBuffer> output = alternate_rgb_buffer(slot, input);
                [encoder setComputePipelineState:context.creative_detail_apply_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:1U];
                [encoder setBuffer:slot.perceptual_small offset:0U atIndex:2U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:3U];
                // Local Contrast is inactive for this specialized stage. Bind valid resident
                // buffers to the dormant inputs so the shared creative kernel has one ABI.
                [encoder setBuffer:slot.perceptual_small offset:0U atIndex:4U];
                [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:5U];
                [encoder setBuffer:output offset:0U atIndex:6U];
                [encoder setBytes:&combined length:sizeof(combined) atIndex:7U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:8U];
                dispatch(context.creative_detail_apply_pipeline());
                return output;
            } else if constexpr (std::is_same_v<Stage, WarmLocalContrastStage>) {
                const auto& small = value.small_box;
                const auto& large = value.large_box;
                const auto& small_coefficients = value.small_coefficients;
                const auto& large_coefficients = value.large_coefficients;
                const auto& creative = value.parameters;
                const WarmTextureParameters lightness{
                    .width = layout.dimensions.width,
                    .height = layout.dimensions.height,
                };
                const auto box_mean = [&](id<MTLBuffer> source,
                                          id<MTLBuffer> horizontal,
                                          id<MTLBuffer> output,
                                          const WarmBoxParameters& parameters) {
                    [encoder setComputePipelineState:context.box_horizontal_pipeline()];
                    [encoder setBuffer:source offset:0U atIndex:0U];
                    [encoder setBuffer:horizontal offset:0U atIndex:1U];
                    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                    dispatch_grid(
                        context.box_horizontal_pipeline(),
                        1U,
                        parameters.height
                    );
                    [encoder setComputePipelineState:context.box_vertical_pipeline()];
                    [encoder setBuffer:horizontal offset:0U atIndex:0U];
                    [encoder setBuffer:output offset:0U atIndex:1U];
                    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                    dispatch_grid(
                        context.box_vertical_pipeline(),
                        parameters.width,
                        1U
                    );
                };
                const auto square = [&](id<MTLBuffer> source,
                                        id<MTLBuffer> output,
                                        const WarmBoxParameters& parameters) {
                    [encoder setComputePipelineState:context.scalar_square_pipeline()];
                    [encoder setBuffer:source offset:0U atIndex:0U];
                    [encoder setBuffer:output offset:0U atIndex:1U];
                    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                    dispatch(context.scalar_square_pipeline());
                };
                const auto coefficients = [&](id<MTLBuffer> guide,
                                              id<MTLBuffer> mean,
                                              id<MTLBuffer> variance,
                                              id<MTLBuffer> a,
                                              id<MTLBuffer> b,
                                              const WarmGuidedCoefficientsParameters& parameters) {
                    [encoder setComputePipelineState:context.guided_coefficients_pipeline()];
                    [encoder setBuffer:guide offset:0U atIndex:0U];
                    [encoder setBuffer:mean offset:0U atIndex:1U];
                    [encoder setBuffer:variance offset:0U atIndex:2U];
                    [encoder setBuffer:a offset:0U atIndex:3U];
                    [encoder setBuffer:b offset:0U atIndex:4U];
                    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5U];
                    dispatch(context.guided_coefficients_pipeline());
                };
                const auto combine = [&](id<MTLBuffer> guide,
                                         id<MTLBuffer> a,
                                         id<MTLBuffer> b,
                                         id<MTLBuffer> output,
                                         const WarmBoxParameters& parameters) {
                    [encoder setComputePipelineState:context.guided_combine_pipeline()];
                    [encoder setBuffer:guide offset:0U atIndex:0U];
                    [encoder setBuffer:a offset:0U atIndex:1U];
                    [encoder setBuffer:b offset:0U atIndex:2U];
                    [encoder setBuffer:output offset:0U atIndex:3U];
                    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4U];
                    dispatch(context.guided_combine_pipeline());
                };

                const id<MTLBuffer> guide = slot.sharpen_log_luminance;
                const id<MTLBuffer> horizontal = slot.sharpen_horizontal;
                const id<MTLBuffer> small_output = slot.perceptual_small;
                const id<MTLBuffer> large_output = slot.perceptual_texture;
                const id<MTLBuffer> scratch_a = slot.local_contrast_a;
                const id<MTLBuffer> scratch_b = slot.local_contrast_b;
                [encoder setComputePipelineState:context.texture_lightness_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:guide offset:0U atIndex:1U];
                [encoder setBytes:&lightness length:sizeof(lightness) atIndex:2U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:3U];
                dispatch(context.texture_lightness_pipeline());

                box_mean(guide, horizontal, small_output, small);
                square(guide, horizontal, small);
                box_mean(horizontal, scratch_a, large_output, small);
                coefficients(
                    guide,
                    small_output,
                    large_output,
                    scratch_a,
                    scratch_b,
                    small_coefficients
                );
                box_mean(scratch_a, horizontal, small_output, small);
                box_mean(scratch_b, horizontal, large_output, small);
                combine(guide, small_output, large_output, small_output, small);

                box_mean(guide, horizontal, large_output, large);
                square(guide, horizontal, large);
                box_mean(horizontal, scratch_a, scratch_b, large);
                coefficients(
                    guide,
                    large_output,
                    scratch_b,
                    scratch_a,
                    horizontal,
                    large_coefficients
                );
                box_mean(scratch_a, scratch_b, scratch_a, large);
                box_mean(horizontal, scratch_b, large_output, large);
                combine(guide, scratch_a, large_output, large_output, large);

                if (value.texture_gaussian.has_value()) {
                    const auto& texture = *value.texture_gaussian;
                    [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                    [encoder setBuffer:guide offset:0U atIndex:0U];
                    [encoder setBuffer:horizontal offset:0U atIndex:1U];
                    [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                    dispatch(context.texture_horizontal_pipeline());
                    [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
                    [encoder setBuffer:horizontal offset:0U atIndex:0U];
                    [encoder setBuffer:scratch_a offset:0U atIndex:1U];
                    [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                    dispatch(context.scalar_vertical_pipeline());
                }
                if (value.clarity_small_gaussian.has_value()
                    && value.clarity_large_gaussian.has_value()) {
                    const auto& clarity_small = *value.clarity_small_gaussian;
                    const auto& clarity_large = *value.clarity_large_gaussian;
                    [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                    [encoder setBuffer:guide offset:0U atIndex:0U];
                    [encoder setBuffer:horizontal offset:0U atIndex:1U];
                    [encoder setBytes:&clarity_small
                               length:sizeof(clarity_small)
                              atIndex:2U];
                    dispatch(context.texture_horizontal_pipeline());
                    [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
                    [encoder setBuffer:horizontal offset:0U atIndex:0U];
                    [encoder setBuffer:scratch_b offset:0U atIndex:1U];
                    [encoder setBytes:&clarity_small
                               length:sizeof(clarity_small)
                              atIndex:2U];
                    dispatch(context.scalar_vertical_pipeline());
                    [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                    [encoder setBuffer:guide offset:0U atIndex:0U];
                    [encoder setBuffer:horizontal offset:0U atIndex:1U];
                    [encoder setBytes:&clarity_large
                               length:sizeof(clarity_large)
                              atIndex:2U];
                    dispatch(context.texture_horizontal_pipeline());
                }

                id<MTLBuffer> output = alternate_rgb_buffer(slot, input);
                [encoder setComputePipelineState:context.creative_detail_apply_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:scratch_a offset:0U atIndex:1U];
                [encoder setBuffer:scratch_b offset:0U atIndex:2U];
                [encoder setBuffer:horizontal offset:0U atIndex:3U];
                [encoder setBuffer:small_output offset:0U atIndex:4U];
                [encoder setBuffer:large_output offset:0U atIndex:5U];
                [encoder setBuffer:output offset:0U atIndex:6U];
                [encoder setBytes:&creative length:sizeof(creative) atIndex:7U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:8U];
                dispatch(context.creative_detail_apply_pipeline());
                return output;
            } else if constexpr (std::is_same_v<Stage, WarmSelectiveToneStage>) {
                const auto& horizontal_box = value.horizontal_box;
                const auto& vertical_box = value.vertical_box;
                const auto& coefficients_parameters = value.coefficients;
                const auto& selective_tone = value.parameters;
                const auto box_mean = [&](id<MTLBuffer> source,
                                          id<MTLBuffer> horizontal,
                                          id<MTLBuffer> output) {
                    [encoder setComputePipelineState:context.reflect_box_horizontal_pipeline()];
                    [encoder setBuffer:source offset:0U atIndex:0U];
                    [encoder setBuffer:horizontal offset:0U atIndex:1U];
                    [encoder setBytes:&horizontal_box
                               length:sizeof(horizontal_box)
                              atIndex:2U];
                    dispatch(context.reflect_box_horizontal_pipeline());
                    [encoder setComputePipelineState:context.reflect_box_vertical_pipeline()];
                    [encoder setBuffer:horizontal offset:0U atIndex:0U];
                    [encoder setBuffer:output offset:0U atIndex:1U];
                    [encoder setBytes:&vertical_box length:sizeof(vertical_box) atIndex:2U];
                    dispatch(context.reflect_box_vertical_pipeline());
                };
                const id<MTLBuffer> guide = slot.sharpen_log_luminance;
                const id<MTLBuffer> horizontal = slot.sharpen_horizontal;
                const id<MTLBuffer> mean = slot.perceptual_small;
                const id<MTLBuffer> mean_square = slot.perceptual_texture;
                const id<MTLBuffer> coefficient_a = slot.local_contrast_a;
                const id<MTLBuffer> coefficient_b = slot.local_contrast_b;

                [encoder setComputePipelineState:context.selective_tone_guide_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:guide offset:0U atIndex:1U];
                [encoder setBytes:&selective_tone length:sizeof(selective_tone) atIndex:2U];
                dispatch(context.selective_tone_guide_pipeline());
                box_mean(guide, horizontal, mean);

                [encoder setComputePipelineState:context.scalar_square_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:horizontal offset:0U atIndex:1U];
                [encoder setBytes:&horizontal_box length:sizeof(horizontal_box) atIndex:2U];
                dispatch(context.scalar_square_pipeline());
                box_mean(horizontal, coefficient_a, mean_square);

                [encoder setComputePipelineState:context.guided_coefficients_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:mean offset:0U atIndex:1U];
                [encoder setBuffer:mean_square offset:0U atIndex:2U];
                [encoder setBuffer:coefficient_a offset:0U atIndex:3U];
                [encoder setBuffer:coefficient_b offset:0U atIndex:4U];
                [encoder setBytes:&coefficients_parameters
                           length:sizeof(coefficients_parameters)
                          atIndex:5U];
                dispatch(context.guided_coefficients_pipeline());
                box_mean(coefficient_a, horizontal, mean);
                box_mean(coefficient_b, horizontal, mean_square);

                [encoder setComputePipelineState:context.guided_combine_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:mean offset:0U atIndex:1U];
                [encoder setBuffer:mean_square offset:0U atIndex:2U];
                [encoder setBuffer:coefficient_a offset:0U atIndex:3U];
                [encoder setBytes:&horizontal_box length:sizeof(horizontal_box) atIndex:4U];
                dispatch(context.guided_combine_pipeline());

                id<MTLBuffer> output = alternate_rgb_buffer(slot, input);
                [encoder setComputePipelineState:context.selective_tone_apply_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:coefficient_a offset:0U atIndex:1U];
                [encoder setBuffer:output offset:0U atIndex:2U];
                [encoder setBuffer:source_highlight_chroma_confidence offset:0U atIndex:3U];
                [encoder setBytes:&selective_tone length:sizeof(selective_tone) atIndex:4U];
                dispatch(context.selective_tone_apply_pipeline());
                return output;
            } else if constexpr (std::is_same_v<Stage, WarmTextureStage>) {
                const auto& texture = value.parameters;
                [encoder setComputePipelineState:context.texture_lightness_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
                [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:3U];
                dispatch(context.texture_lightness_pipeline());

                [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
                dispatch(context.texture_horizontal_pipeline());

                id<MTLBuffer> output = alternate_rgb_buffer(slot, input);
                [encoder setComputePipelineState:context.texture_apply_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBuffer:output offset:0U atIndex:2U];
                [encoder setBytes:&texture length:sizeof(texture) atIndex:3U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:4U];
                dispatch(context.texture_apply_pipeline());
                return output;
            } else if constexpr (std::is_same_v<Stage, WarmClarityStage>) {
                const auto& small = value.small_gaussian;
                const auto& large = value.large_gaussian;
                const auto& clarity = value.parameters;
                [encoder setComputePipelineState:context.texture_lightness_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
                [encoder setBytes:&small length:sizeof(small) atIndex:2U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:3U];
                dispatch(context.texture_lightness_pipeline());

                [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&small length:sizeof(small) atIndex:2U];
                dispatch(context.texture_horizontal_pipeline());
                [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
                [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
                [encoder setBytes:&small length:sizeof(small) atIndex:2U];
                dispatch(context.scalar_vertical_pipeline());

                [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&large length:sizeof(large) atIndex:2U];
                dispatch(context.texture_horizontal_pipeline());

                id<MTLBuffer> output = alternate_rgb_buffer(slot, input);
                [encoder setComputePipelineState:context.clarity_apply_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:2U];
                [encoder setBuffer:output offset:0U atIndex:3U];
                [encoder setBytes:&clarity length:sizeof(clarity) atIndex:4U];
                [encoder setBytes:&color_invocation
                           length:sizeof(color_invocation)
                          atIndex:5U];
                dispatch(context.clarity_apply_pipeline());
                return output;
            }
        },
        stage
    );
}

} // namespace shadow::image::detail
