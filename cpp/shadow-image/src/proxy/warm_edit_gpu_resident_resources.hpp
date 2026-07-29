#pragma once

// This contract is Objective-C++ only. Isolate the legacy MacTypes `shadow` token while
// importing the Apple runtime so it cannot collide with Shadow's C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "warm_edit_gpu_kernel_contract.hpp"

namespace shadow::image {
struct WarmEditPreviewGpuStats;
}

namespace shadow::image::detail {

struct PreparedMetalAdjustment;

struct WarmGpuResidentLayout final {
    Dimensions dimensions;
    std::size_t source_row_stride_bytes = 0U;
    std::size_t adjusted_row_stride_bytes = 0U;
    std::size_t adjusted_sample_count = 0U;
    std::size_t adjusted_bytes = 0U;
    std::size_t rgb8_bytes = 0U;
    FloatPixelFormat pixel_format = FloatPixelFormat::unknown;
    TransferFunction transfer_function = TransferFunction::unknown;
    ImageReference reference = ImageReference::unknown;
    WorkingRgbSpace working_space;
    double level_zero_to_raster_scale_x = 1.0;
    double level_zero_to_raster_scale_y = 1.0;
};

class RetainedMetalBuffer final {
  public:
    RetainedMetalBuffer() noexcept = default;
    explicit RetainedMetalBuffer(id<MTLBuffer> value) noexcept;
    ~RetainedMetalBuffer();

    RetainedMetalBuffer(const RetainedMetalBuffer&) = delete;
    RetainedMetalBuffer& operator=(const RetainedMetalBuffer&) = delete;
    RetainedMetalBuffer(RetainedMetalBuffer&& other) noexcept;
    RetainedMetalBuffer& operator=(RetainedMetalBuffer&& other) noexcept;

    [[nodiscard]] id<MTLBuffer> get() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

  private:
    id<MTLBuffer> value_ = nil;
};

struct WarmProgramBuffers final {
    RetainedMetalBuffer curve;
    RetainedMetalBuffer lut;
    RetainedMetalBuffer perceptual_mixer;
    RetainedMetalBuffer perceptual_range;
    RetainedMetalBuffer selective_color;
};

struct WarmProgramBufferAttempt final {
    WarmProgramBuffers buffers;
    bool cancelled = false;
    std::string diagnostic;
};

struct WarmBrushBufferAttempt final {
    RetainedMetalBuffer buffer;
    bool cancelled = false;
    std::string diagnostic;
};

struct WarmRetouchBufferAttempt final {
    RetainedMetalBuffer buffer;
    bool cancelled = false;
    std::string diagnostic;
};

struct WarmGpuSlotBuffers final {
    id<MTLBuffer> adjusted = nil;
    id<MTLBuffer> denoised = nil;
    id<MTLBuffer> sharpen_log_luminance = nil;
    id<MTLBuffer> sharpen_horizontal = nil;
    id<MTLBuffer> perceptual_small = nil;
    id<MTLBuffer> perceptual_texture = nil;
    id<MTLBuffer> local_contrast_a = nil;
    id<MTLBuffer> local_contrast_b = nil;
    id<MTLBuffer> layer_before = nil;
    id<MTLBuffer> rgb8 = nil;
    id<MTLBuffer> before_operations = nil;
    id<MTLBuffer> after_operations = nil;
    id<MTLBuffer> status = nil;
};

class WarmGpuResidentResources;

class WarmGpuSlotLease final {
  public:
    WarmGpuSlotLease(const WarmGpuSlotLease&) = delete;
    WarmGpuSlotLease& operator=(const WarmGpuSlotLease&) = delete;
    WarmGpuSlotLease(WarmGpuSlotLease&& other) noexcept;
    WarmGpuSlotLease& operator=(WarmGpuSlotLease&&) = delete;
    ~WarmGpuSlotLease();

    [[nodiscard]] WarmGpuSlotBuffers buffers() const noexcept;
    [[nodiscard]] std::string ensure_denoise_resources();
    [[nodiscard]] std::string ensure_sharpen_resources();
    [[nodiscard]] std::string ensure_clarity_resources();
    [[nodiscard]] std::string ensure_texture_clarity_resources();
    [[nodiscard]] std::string ensure_local_contrast_resources();
    [[nodiscard]] std::string ensure_layer_resources();
    void mark_completed() noexcept;

  private:
    WarmGpuSlotLease(WarmGpuResidentResources& owner, std::size_t index) noexcept;

    WarmGpuResidentResources* owner_ = nullptr;
    std::size_t index_ = 0U;
    bool completed_ = false;

    friend class WarmGpuResidentResources;
};

struct WarmGpuResidentPreparation;

class WarmGpuResidentResources final {
  public:
    WarmGpuResidentResources(const WarmGpuResidentResources&) = delete;
    WarmGpuResidentResources& operator=(const WarmGpuResidentResources&) = delete;
    ~WarmGpuResidentResources();

    [[nodiscard]] const WarmGpuResidentLayout& layout() const noexcept;
    [[nodiscard]] id<MTLBuffer> source_buffer() const noexcept;
    [[nodiscard]] std::size_t operation_capacity() const noexcept;

    [[nodiscard]] WarmProgramBufferAttempt
    acquire_program_buffers(const PreparedMetalAdjustment& program, std::stop_token cancellation);
    [[nodiscard]] WarmBrushBufferAttempt acquire_brush_index_buffer(
        const std::vector<std::uint32_t>& words,
        std::stop_token cancellation
    );
    [[nodiscard]] WarmRetouchBufferAttempt acquire_retouch_geometry_buffer(
        const std::vector<WarmRetouchWord>& words,
        std::stop_token cancellation
    );
    [[nodiscard]] std::optional<WarmGpuSlotLease> acquire_slot(std::stop_token cancellation);
    [[nodiscard]] WarmEditPreviewGpuStats stats_snapshot() const noexcept;

  private:
    struct Impl;
    explicit WarmGpuResidentResources(std::unique_ptr<Impl> impl);

    [[nodiscard]] WarmGpuSlotBuffers slot_buffers(std::size_t index) const noexcept;
    [[nodiscard]] std::string ensure_denoise_resources(std::size_t index);
    [[nodiscard]] std::string ensure_sharpen_resources(std::size_t index);
    [[nodiscard]] std::string ensure_clarity_resources(std::size_t index);
    [[nodiscard]] std::string ensure_texture_clarity_resources(std::size_t index);
    [[nodiscard]] std::string ensure_local_contrast_resources(std::size_t index);
    [[nodiscard]] std::string ensure_layer_resources(std::size_t index);
    void release_slot(std::size_t index, bool completed) noexcept;

    std::unique_ptr<Impl> impl_;

    friend class WarmGpuSlotLease;
    friend WarmGpuResidentPreparation
    prepare_warm_gpu_resident_resources(const FloatRgbImage& source, id<MTLDevice> device);
};

struct WarmGpuResidentPreparation final {
    std::unique_ptr<WarmGpuResidentResources> resources;
    std::string diagnostic;
};

[[nodiscard]] WarmGpuResidentPreparation
prepare_warm_gpu_resident_resources(const FloatRgbImage& source, id<MTLDevice> device);

} // namespace shadow::image::detail
