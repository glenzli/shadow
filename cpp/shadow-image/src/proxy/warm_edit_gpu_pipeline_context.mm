#include "warm_edit_gpu_pipeline_context.hpp"

#include "../edit/metal_adjustment_msl.hpp"
#include "warm_edit_gpu_geometry_msl.hpp"
#include "warm_edit_gpu_msl.hpp"

#include <string>

namespace shadow::image::detail {

namespace {

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

} // namespace

WarmMetalContext::WarmMetalContext() {
    @autoreleasepool {
        device_ = MTLCreateSystemDefaultDevice();
        if (device_ == nil) {
            diagnostic_ = "no Metal device is available";
            return;
        }
        queue_ = [device_ newCommandQueue];
        if (queue_ == nil) {
            diagnostic_ = "Metal could not create a warm-preview command queue";
            return;
        }

        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.mathMode = MTLMathModeSafe;
        NSError* error = nil;
        std::string warm_source;
        warm_source.reserve(
            warm_kernel_source_prefix.size() + warm_retouch_kernel_source.size()
            + warm_geometry_kernel_source.size() + warm_kernel_source_suffix.size()
        );
        warm_source.append(warm_kernel_source_prefix);
        warm_source.append(warm_retouch_kernel_source);
        warm_source.append(warm_geometry_kernel_source);
        warm_source.append(warm_kernel_source_suffix);
        const std::string metal_source = make_metal_adjustment_source(warm_source);
        NSString* source = [[NSString alloc] initWithBytes:metal_source.data()
                                                    length:metal_source.size()
                                                  encoding:NSUTF8StringEncoding];
        if (source == nil) {
            [options release];
            diagnostic_ = "Metal warm-preview shader source is not valid UTF-8";
            return;
        }
        id<MTLLibrary> library = [device_ newLibraryWithSource:source options:options error:&error];
        [source release];
        [options release];
        if (library == nil) {
            diagnostic_ =
                "Metal warm-preview shader compilation failed: " + error_description(error);
            return;
        }
        id<MTLFunction> display_function = [library newFunctionWithName:@"render_warm_preview_v1"];
        id<MTLFunction> adjustment_function =
            [library newFunctionWithName:@"execute_warm_adjustment_v1"];
        id<MTLFunction> denoise_function = [library newFunctionWithName:@"guided_denoise_warm_v1"];
        id<MTLFunction> sharpen_log_function =
            [library newFunctionWithName:@"warm_sharpen_log_luminance_v1"];
        id<MTLFunction> sharpen_horizontal_function =
            [library newFunctionWithName:@"warm_sharpen_horizontal_v1"];
        id<MTLFunction> sharpen_apply_function =
            [library newFunctionWithName:@"warm_sharpen_apply_v1"];
        id<MTLFunction> texture_lightness_function =
            [library newFunctionWithName:@"warm_texture_lightness_v1"];
        id<MTLFunction> texture_horizontal_function =
            [library newFunctionWithName:@"warm_texture_horizontal_v1"];
        id<MTLFunction> texture_apply_function =
            [library newFunctionWithName:@"warm_texture_apply_v1"];
        id<MTLFunction> scalar_vertical_function =
            [library newFunctionWithName:@"warm_scalar_vertical_v1"];
        id<MTLFunction> clarity_apply_function =
            [library newFunctionWithName:@"warm_clarity_apply_v1"];
        if (display_function == nil || adjustment_function == nil || denoise_function == nil ||
            sharpen_log_function == nil || sharpen_horizontal_function == nil ||
            sharpen_apply_function == nil || texture_lightness_function == nil ||
            texture_horizontal_function == nil || texture_apply_function == nil ||
            scalar_vertical_function == nil || clarity_apply_function == nil) {
            [display_function release];
            [adjustment_function release];
            [denoise_function release];
            [sharpen_log_function release];
            [sharpen_horizontal_function release];
            [sharpen_apply_function release];
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview shader entry point is unavailable";
            return;
        }
        display_pipeline_ = [device_ newComputePipelineStateWithFunction:display_function
                                                                   error:&error];
        [display_function release];
        if (display_pipeline_ == nil) {
            [adjustment_function release];
            [denoise_function release];
            [sharpen_log_function release];
            [sharpen_horizontal_function release];
            [sharpen_apply_function release];
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ =
                "Metal warm-preview pipeline creation failed: " + error_description(error);
            return;
        }
        adjustment_pipeline_ = [device_ newComputePipelineStateWithFunction:adjustment_function
                                                                      error:&error];
        [adjustment_function release];
        if (adjustment_pipeline_ == nil) {
            [denoise_function release];
            [sharpen_log_function release];
            [sharpen_horizontal_function release];
            [sharpen_apply_function release];
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview adjustment pipeline creation failed: " +
                          error_description(error);
            return;
        }
        denoise_pipeline_ = [device_ newComputePipelineStateWithFunction:denoise_function
                                                                   error:&error];
        [denoise_function release];
        if (denoise_pipeline_ == nil) {
            [sharpen_log_function release];
            [sharpen_horizontal_function release];
            [sharpen_apply_function release];
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ =
                "Metal warm-preview denoise pipeline creation failed: " + error_description(error);
            return;
        }
        sharpen_log_pipeline_ = [device_ newComputePipelineStateWithFunction:sharpen_log_function
                                                                       error:&error];
        [sharpen_log_function release];
        if (sharpen_log_pipeline_ == nil) {
            [sharpen_horizontal_function release];
            [sharpen_apply_function release];
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview sharpen-log pipeline creation failed: " +
                          error_description(error);
            return;
        }
        sharpen_horizontal_pipeline_ =
            [device_ newComputePipelineStateWithFunction:sharpen_horizontal_function error:&error];
        [sharpen_horizontal_function release];
        if (sharpen_horizontal_pipeline_ == nil) {
            [sharpen_apply_function release];
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview sharpen-horizontal pipeline creation failed: " +
                          error_description(error);
            return;
        }
        sharpen_apply_pipeline_ =
            [device_ newComputePipelineStateWithFunction:sharpen_apply_function error:&error];
        [sharpen_apply_function release];
        if (sharpen_apply_pipeline_ == nil) {
            [texture_lightness_function release];
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview sharpen-apply pipeline creation failed: " +
                          error_description(error);
            return;
        }
        texture_lightness_pipeline_ =
            [device_ newComputePipelineStateWithFunction:texture_lightness_function error:&error];
        [texture_lightness_function release];
        if (texture_lightness_pipeline_ == nil) {
            [texture_horizontal_function release];
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview texture-lightness pipeline creation failed: " +
                          error_description(error);
            return;
        }
        texture_horizontal_pipeline_ =
            [device_ newComputePipelineStateWithFunction:texture_horizontal_function error:&error];
        [texture_horizontal_function release];
        if (texture_horizontal_pipeline_ == nil) {
            [texture_apply_function release];
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview texture-horizontal pipeline creation failed: " +
                          error_description(error);
            return;
        }
        texture_apply_pipeline_ =
            [device_ newComputePipelineStateWithFunction:texture_apply_function error:&error];
        [texture_apply_function release];
        if (texture_apply_pipeline_ == nil) {
            [scalar_vertical_function release];
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview texture-apply pipeline creation failed: " +
                          error_description(error);
            return;
        }
        scalar_vertical_pipeline_ =
            [device_ newComputePipelineStateWithFunction:scalar_vertical_function error:&error];
        [scalar_vertical_function release];
        if (scalar_vertical_pipeline_ == nil) {
            [clarity_apply_function release];
            [library release];
            diagnostic_ = "Metal warm-preview scalar-vertical pipeline creation failed: " +
                          error_description(error);
            return;
        }
        clarity_apply_pipeline_ =
            [device_ newComputePipelineStateWithFunction:clarity_apply_function error:&error];
        [clarity_apply_function release];
        if (clarity_apply_pipeline_ == nil) {
            [library release];
            diagnostic_ = "Metal warm-preview clarity-apply pipeline creation failed: " +
                          error_description(error);
            return;
        }
        id<MTLFunction> dehaze_defringe_function =
            [library newFunctionWithName:@"warm_dehaze_defringe_v1"];
        if (dehaze_defringe_function == nil) {
            [library release];
            diagnostic_ = "Metal warm-preview dehaze-defringe shader entry point is "
                          "unavailable";
            return;
        }
        dehaze_defringe_pipeline_ =
            [device_ newComputePipelineStateWithFunction:dehaze_defringe_function error:&error];
        [dehaze_defringe_function release];
        if (dehaze_defringe_pipeline_ == nil) {
            [library release];
            diagnostic_ = "Metal warm-preview dehaze-defringe pipeline creation failed: " +
                          error_description(error);
            return;
        }
        id<MTLFunction> creative_detail_apply_function =
            [library newFunctionWithName:@"warm_creative_detail_apply_v1"];
        if (creative_detail_apply_function == nil) {
            [library release];
            diagnostic_ = "Metal warm-preview creative-detail shader entry point is "
                          "unavailable";
            return;
        }
        creative_detail_apply_pipeline_ =
            [device_ newComputePipelineStateWithFunction:creative_detail_apply_function
                                                   error:&error];
        [creative_detail_apply_function release];
        if (creative_detail_apply_pipeline_ == nil) {
            [library release];
            diagnostic_ = "Metal warm-preview creative-detail pipeline creation failed: " +
                          error_description(error);
            return;
        }
        id<MTLFunction> box_horizontal_function =
            [library newFunctionWithName:@"warm_box_horizontal_v1"];
        id<MTLFunction> box_vertical_function =
            [library newFunctionWithName:@"warm_box_vertical_v1"];
        id<MTLFunction> scalar_square_function =
            [library newFunctionWithName:@"warm_scalar_square_v1"];
        id<MTLFunction> guided_coefficients_function =
            [library newFunctionWithName:@"warm_guided_coefficients_v1"];
        id<MTLFunction> guided_combine_function =
            [library newFunctionWithName:@"warm_guided_combine_v1"];
        id<MTLFunction> selective_tone_guide_function =
            [library newFunctionWithName:@"warm_selective_tone_guide_v1"];
        id<MTLFunction> reflect_box_horizontal_function =
            [library newFunctionWithName:@"warm_reflect_box_horizontal_v1"];
        id<MTLFunction> reflect_box_vertical_function =
            [library newFunctionWithName:@"warm_reflect_box_vertical_v1"];
        id<MTLFunction> selective_tone_apply_function =
            [library newFunctionWithName:@"warm_selective_tone_apply_v1"];
        id<MTLFunction> layer_copy_function =
            [library newFunctionWithName:@"warm_copy_rgb_v1"];
        id<MTLFunction> layer_blend_function =
            [library newFunctionWithName:@"warm_layer_blend_v1"];
        id<MTLFunction> geometry_function =
            [library newFunctionWithName:@"warm_photo_geometry_v1"];
        id<MTLFunction> retouch_clone_function =
            [library newFunctionWithName:@"warm_retouch_clone_v1"];
        id<MTLFunction> retouch_heal_statistics_function =
            [library newFunctionWithName:@"warm_retouch_heal_statistics_v1"];
        id<MTLFunction> retouch_heal_reduce_function =
            [library newFunctionWithName:@"warm_retouch_heal_reduce_v1"];
        id<MTLFunction> retouch_heal_initialize_function =
            [library newFunctionWithName:@"warm_retouch_heal_initialize_v1"];
        id<MTLFunction> retouch_heal_jacobi_function =
            [library newFunctionWithName:@"warm_retouch_heal_jacobi_v1"];
        id<MTLFunction> retouch_heal_blend_function =
            [library newFunctionWithName:@"warm_retouch_heal_blend_v1"];
        if (box_horizontal_function == nil || box_vertical_function == nil ||
            scalar_square_function == nil || guided_coefficients_function == nil ||
            guided_combine_function == nil || selective_tone_guide_function == nil
            || reflect_box_horizontal_function == nil ||
            reflect_box_vertical_function == nil || selective_tone_apply_function == nil ||
            layer_copy_function == nil || layer_blend_function == nil
            || geometry_function == nil
            || retouch_clone_function == nil
            || retouch_heal_statistics_function == nil
            || retouch_heal_reduce_function == nil
            || retouch_heal_initialize_function == nil
            || retouch_heal_jacobi_function == nil
            || retouch_heal_blend_function == nil) {
            [box_horizontal_function release];
            [box_vertical_function release];
            [scalar_square_function release];
            [guided_coefficients_function release];
            [guided_combine_function release];
            [selective_tone_guide_function release];
            [reflect_box_horizontal_function release];
            [reflect_box_vertical_function release];
            [selective_tone_apply_function release];
            [layer_copy_function release];
            [layer_blend_function release];
            [geometry_function release];
            [retouch_clone_function release];
            [retouch_heal_statistics_function release];
            [retouch_heal_reduce_function release];
            [retouch_heal_initialize_function release];
            [retouch_heal_jacobi_function release];
            [retouch_heal_blend_function release];
            [library release];
            diagnostic_ =
                "Metal warm-preview guided-stage shader entry point is unavailable";
            return;
        }
        box_horizontal_pipeline_ =
            [device_ newComputePipelineStateWithFunction:box_horizontal_function error:&error];
        [box_horizontal_function release];
        box_vertical_pipeline_ = [device_ newComputePipelineStateWithFunction:box_vertical_function
                                                                        error:&error];
        [box_vertical_function release];
        scalar_square_pipeline_ =
            [device_ newComputePipelineStateWithFunction:scalar_square_function error:&error];
        [scalar_square_function release];
        guided_coefficients_pipeline_ =
            [device_ newComputePipelineStateWithFunction:guided_coefficients_function error:&error];
        [guided_coefficients_function release];
        guided_combine_pipeline_ =
            [device_ newComputePipelineStateWithFunction:guided_combine_function error:&error];
        [guided_combine_function release];
        selective_tone_guide_pipeline_ =
            [device_ newComputePipelineStateWithFunction:selective_tone_guide_function
                                                   error:&error];
        [selective_tone_guide_function release];
        reflect_box_horizontal_pipeline_ =
            [device_ newComputePipelineStateWithFunction:reflect_box_horizontal_function
                                                   error:&error];
        [reflect_box_horizontal_function release];
        reflect_box_vertical_pipeline_ =
            [device_ newComputePipelineStateWithFunction:reflect_box_vertical_function
                                                   error:&error];
        [reflect_box_vertical_function release];
        selective_tone_apply_pipeline_ =
            [device_ newComputePipelineStateWithFunction:selective_tone_apply_function
                                                   error:&error];
        [selective_tone_apply_function release];
        layer_copy_pipeline_ =
            [device_ newComputePipelineStateWithFunction:layer_copy_function error:&error];
        [layer_copy_function release];
        layer_blend_pipeline_ =
            [device_ newComputePipelineStateWithFunction:layer_blend_function error:&error];
        [layer_blend_function release];
        geometry_pipeline_ =
            [device_ newComputePipelineStateWithFunction:geometry_function error:&error];
        [geometry_function release];
        retouch_clone_pipeline_ =
            [device_ newComputePipelineStateWithFunction:retouch_clone_function error:&error];
        [retouch_clone_function release];
        retouch_heal_statistics_pipeline_ = [device_
            newComputePipelineStateWithFunction:retouch_heal_statistics_function
            error:&error];
        [retouch_heal_statistics_function release];
        retouch_heal_reduce_pipeline_ = [device_
            newComputePipelineStateWithFunction:retouch_heal_reduce_function
            error:&error];
        [retouch_heal_reduce_function release];
        retouch_heal_initialize_pipeline_ = [device_
            newComputePipelineStateWithFunction:retouch_heal_initialize_function
            error:&error];
        [retouch_heal_initialize_function release];
        retouch_heal_jacobi_pipeline_ = [device_
            newComputePipelineStateWithFunction:retouch_heal_jacobi_function
            error:&error];
        [retouch_heal_jacobi_function release];
        retouch_heal_blend_pipeline_ = [device_
            newComputePipelineStateWithFunction:retouch_heal_blend_function
            error:&error];
        [retouch_heal_blend_function release];
        [library release];
        if (box_horizontal_pipeline_ == nil || box_vertical_pipeline_ == nil ||
            scalar_square_pipeline_ == nil || guided_coefficients_pipeline_ == nil ||
            guided_combine_pipeline_ == nil || selective_tone_guide_pipeline_ == nil ||
            reflect_box_horizontal_pipeline_ == nil || reflect_box_vertical_pipeline_ == nil ||
            selective_tone_apply_pipeline_ == nil || layer_copy_pipeline_ == nil ||
            layer_blend_pipeline_ == nil || geometry_pipeline_ == nil
            || retouch_clone_pipeline_ == nil
            || retouch_heal_statistics_pipeline_ == nil
            || retouch_heal_reduce_pipeline_ == nil
            || retouch_heal_initialize_pipeline_ == nil
            || retouch_heal_jacobi_pipeline_ == nil
            || retouch_heal_blend_pipeline_ == nil) {
            diagnostic_ = "Metal warm-preview guided/layer pipeline creation failed: " +
                          error_description(error);
        }
    }
}

WarmMetalContext::~WarmMetalContext() {
    [retouch_heal_blend_pipeline_ release];
    [retouch_heal_jacobi_pipeline_ release];
    [retouch_heal_initialize_pipeline_ release];
    [retouch_heal_reduce_pipeline_ release];
    [retouch_heal_statistics_pipeline_ release];
    [retouch_clone_pipeline_ release];
    [geometry_pipeline_ release];
    [layer_blend_pipeline_ release];
    [layer_copy_pipeline_ release];
    [selective_tone_apply_pipeline_ release];
    [reflect_box_vertical_pipeline_ release];
    [reflect_box_horizontal_pipeline_ release];
    [selective_tone_guide_pipeline_ release];
    [guided_combine_pipeline_ release];
    [guided_coefficients_pipeline_ release];
    [scalar_square_pipeline_ release];
    [box_vertical_pipeline_ release];
    [box_horizontal_pipeline_ release];
    [creative_detail_apply_pipeline_ release];
    [dehaze_defringe_pipeline_ release];
    [clarity_apply_pipeline_ release];
    [scalar_vertical_pipeline_ release];
    [texture_apply_pipeline_ release];
    [texture_horizontal_pipeline_ release];
    [texture_lightness_pipeline_ release];
    [sharpen_apply_pipeline_ release];
    [sharpen_horizontal_pipeline_ release];
    [sharpen_log_pipeline_ release];
    [denoise_pipeline_ release];
    [adjustment_pipeline_ release];
    [display_pipeline_ release];
    [queue_ release];
    [device_ release];
}

bool WarmMetalContext::valid() const noexcept {
    return device_ != nil && queue_ != nil && display_pipeline_ != nil &&
           adjustment_pipeline_ != nil && layer_copy_pipeline_ != nil &&
           layer_blend_pipeline_ != nil && geometry_pipeline_ != nil
           && retouch_clone_pipeline_ != nil &&
           retouch_heal_statistics_pipeline_ != nil
           && retouch_heal_reduce_pipeline_ != nil
           && retouch_heal_initialize_pipeline_ != nil
           && retouch_heal_jacobi_pipeline_ != nil
           && retouch_heal_blend_pipeline_ != nil &&
           denoise_pipeline_ != nil &&
           sharpen_log_pipeline_ != nil && sharpen_horizontal_pipeline_ != nil &&
           sharpen_apply_pipeline_ != nil && texture_lightness_pipeline_ != nil &&
           texture_horizontal_pipeline_ != nil && texture_apply_pipeline_ != nil &&
           scalar_vertical_pipeline_ != nil && clarity_apply_pipeline_ != nil &&
           dehaze_defringe_pipeline_ != nil && creative_detail_apply_pipeline_ != nil &&
           box_horizontal_pipeline_ != nil && box_vertical_pipeline_ != nil &&
           scalar_square_pipeline_ != nil && guided_coefficients_pipeline_ != nil &&
           guided_combine_pipeline_ != nil && selective_tone_guide_pipeline_ != nil &&
           reflect_box_horizontal_pipeline_ != nil &&
           reflect_box_vertical_pipeline_ != nil && selective_tone_apply_pipeline_ != nil;
}

id<MTLDevice> WarmMetalContext::device() const noexcept { return device_; }

id<MTLCommandQueue> WarmMetalContext::queue() const noexcept { return queue_; }

id<MTLComputePipelineState> WarmMetalContext::display_pipeline() const noexcept {
    return display_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::adjustment_pipeline() const noexcept {
    return adjustment_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::layer_copy_pipeline() const noexcept {
    return layer_copy_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::layer_blend_pipeline() const noexcept {
    return layer_blend_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::geometry_pipeline() const noexcept {
    return geometry_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::retouch_clone_pipeline() const noexcept {
    return retouch_clone_pipeline_;
}

id<MTLComputePipelineState>
WarmMetalContext::retouch_heal_statistics_pipeline() const noexcept {
    return retouch_heal_statistics_pipeline_;
}

id<MTLComputePipelineState>
WarmMetalContext::retouch_heal_reduce_pipeline() const noexcept {
    return retouch_heal_reduce_pipeline_;
}

id<MTLComputePipelineState>
WarmMetalContext::retouch_heal_initialize_pipeline() const noexcept {
    return retouch_heal_initialize_pipeline_;
}

id<MTLComputePipelineState>
WarmMetalContext::retouch_heal_jacobi_pipeline() const noexcept {
    return retouch_heal_jacobi_pipeline_;
}

id<MTLComputePipelineState>
WarmMetalContext::retouch_heal_blend_pipeline() const noexcept {
    return retouch_heal_blend_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::denoise_pipeline() const noexcept {
    return denoise_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::sharpen_log_pipeline() const noexcept {
    return sharpen_log_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::sharpen_horizontal_pipeline() const noexcept {
    return sharpen_horizontal_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::sharpen_apply_pipeline() const noexcept {
    return sharpen_apply_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::texture_lightness_pipeline() const noexcept {
    return texture_lightness_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::texture_horizontal_pipeline() const noexcept {
    return texture_horizontal_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::texture_apply_pipeline() const noexcept {
    return texture_apply_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::scalar_vertical_pipeline() const noexcept {
    return scalar_vertical_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::clarity_apply_pipeline() const noexcept {
    return clarity_apply_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::dehaze_defringe_pipeline() const noexcept {
    return dehaze_defringe_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::creative_detail_apply_pipeline() const noexcept {
    return creative_detail_apply_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::box_horizontal_pipeline() const noexcept {
    return box_horizontal_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::box_vertical_pipeline() const noexcept {
    return box_vertical_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::scalar_square_pipeline() const noexcept {
    return scalar_square_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::guided_coefficients_pipeline() const noexcept {
    return guided_coefficients_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::guided_combine_pipeline() const noexcept {
    return guided_combine_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::selective_tone_guide_pipeline() const noexcept {
    return selective_tone_guide_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::reflect_box_horizontal_pipeline() const noexcept {
    return reflect_box_horizontal_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::reflect_box_vertical_pipeline() const noexcept {
    return reflect_box_vertical_pipeline_;
}

id<MTLComputePipelineState> WarmMetalContext::selective_tone_apply_pipeline() const noexcept {
    return selective_tone_apply_pipeline_;
}

const std::string& WarmMetalContext::diagnostic() const noexcept { return diagnostic_; }

std::string command_buffer_diagnostic(id<MTLCommandBuffer> command_buffer) {
    const std::string detail = error_description(command_buffer.error);
    return detail.empty() ? "Metal warm-preview command did not complete successfully"
                          : "Metal warm-preview command failed: " + detail;
}

WarmMetalContext& metal_context() {
    static WarmMetalContext context;
    return context;
}

} // namespace shadow::image::detail
