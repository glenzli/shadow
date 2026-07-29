#pragma once

// This contract is Objective-C++ only. Isolate the legacy MacTypes `shadow`
// token while importing the Apple runtime so it cannot collide with Shadow's
// C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include <string>

namespace shadow::image::detail {

class WarmMetalContext final {
  public:
    WarmMetalContext();
    ~WarmMetalContext();

    WarmMetalContext(const WarmMetalContext&) = delete;
    WarmMetalContext& operator=(const WarmMetalContext&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] id<MTLDevice> device() const noexcept;
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> display_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> adjustment_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> layer_copy_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> layer_blend_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> mask_coverage_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> mask_coverage_geometry_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> geometry_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> retouch_clone_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> retouch_heal_statistics_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> retouch_heal_reduce_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> retouch_heal_initialize_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> retouch_heal_jacobi_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> retouch_heal_blend_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> denoise_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> sharpen_log_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> sharpen_horizontal_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> sharpen_apply_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> texture_lightness_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> texture_horizontal_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> texture_apply_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> scalar_vertical_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> clarity_apply_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> dehaze_defringe_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> creative_detail_apply_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> box_horizontal_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> box_vertical_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> scalar_square_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> guided_coefficients_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> guided_combine_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> selective_tone_guide_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> reflect_box_horizontal_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> reflect_box_vertical_pipeline() const noexcept;
    [[nodiscard]] id<MTLComputePipelineState> selective_tone_apply_pipeline() const noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept;

  private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> display_pipeline_ = nil;
    id<MTLComputePipelineState> adjustment_pipeline_ = nil;
    id<MTLComputePipelineState> layer_copy_pipeline_ = nil;
    id<MTLComputePipelineState> layer_blend_pipeline_ = nil;
    id<MTLComputePipelineState> mask_coverage_pipeline_ = nil;
    id<MTLComputePipelineState> mask_coverage_geometry_pipeline_ = nil;
    id<MTLComputePipelineState> geometry_pipeline_ = nil;
    id<MTLComputePipelineState> retouch_clone_pipeline_ = nil;
    id<MTLComputePipelineState> retouch_heal_statistics_pipeline_ = nil;
    id<MTLComputePipelineState> retouch_heal_reduce_pipeline_ = nil;
    id<MTLComputePipelineState> retouch_heal_initialize_pipeline_ = nil;
    id<MTLComputePipelineState> retouch_heal_jacobi_pipeline_ = nil;
    id<MTLComputePipelineState> retouch_heal_blend_pipeline_ = nil;
    id<MTLComputePipelineState> denoise_pipeline_ = nil;
    id<MTLComputePipelineState> sharpen_log_pipeline_ = nil;
    id<MTLComputePipelineState> sharpen_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> sharpen_apply_pipeline_ = nil;
    id<MTLComputePipelineState> texture_lightness_pipeline_ = nil;
    id<MTLComputePipelineState> texture_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> texture_apply_pipeline_ = nil;
    id<MTLComputePipelineState> scalar_vertical_pipeline_ = nil;
    id<MTLComputePipelineState> clarity_apply_pipeline_ = nil;
    id<MTLComputePipelineState> dehaze_defringe_pipeline_ = nil;
    id<MTLComputePipelineState> creative_detail_apply_pipeline_ = nil;
    id<MTLComputePipelineState> box_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> box_vertical_pipeline_ = nil;
    id<MTLComputePipelineState> scalar_square_pipeline_ = nil;
    id<MTLComputePipelineState> guided_coefficients_pipeline_ = nil;
    id<MTLComputePipelineState> guided_combine_pipeline_ = nil;
    id<MTLComputePipelineState> selective_tone_guide_pipeline_ = nil;
    id<MTLComputePipelineState> reflect_box_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> reflect_box_vertical_pipeline_ = nil;
    id<MTLComputePipelineState> selective_tone_apply_pipeline_ = nil;
    std::string diagnostic_;
};

// Device, queue, compiled library, and immutable pipeline states form one
// process-wide all-or-nothing context. Mutable rasters and slots remain
// session-owned.
[[nodiscard]] WarmMetalContext& metal_context();

[[nodiscard]] std::string command_buffer_diagnostic(id<MTLCommandBuffer> command_buffer);

} // namespace shadow::image::detail
