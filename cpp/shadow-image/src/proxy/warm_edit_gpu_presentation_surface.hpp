#pragma once

#include <shadow/image/decoder_types.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if defined(__OBJC__)
@protocol MTLBuffer;
@protocol MTLCommandBuffer;
@protocol MTLDevice;
#endif

namespace shadow::image::detail {

struct WarmEditGpuPresentationSurfacePreparation;

// The warm renderer's display kernel produces display-sRGB-encoded eight-bit values. Qt Quick
// imports this surface as an opaque native texture and samples the stored values directly for its
// ordinary preview composition. Keep the physical Metal view unorm: an `_sRGB` view would decode
// the already display-encoded bytes during sampling and make the interactive frame disagree with
// the settled QImage/JPEG frame.
enum class WarmEditGpuPresentationPixelFormat : std::uint8_t {
    rgba8_unorm_display_srgb = 1U,
};

// One completed warm-preview presentation allocation. It owns a dedicated shared Metal buffer
// and a two-dimensional buffer-backed texture; neither resource aliases either resident render
// slot. The native handles are borrowed from this owner and remain valid until it is destroyed.
class WarmEditGpuPresentationSurface final {
  public:
    ~WarmEditGpuPresentationSurface();

    WarmEditGpuPresentationSurface(const WarmEditGpuPresentationSurface&) = delete;
    WarmEditGpuPresentationSurface& operator=(const WarmEditGpuPresentationSurface&) = delete;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t row_stride_bytes() const noexcept;
    [[nodiscard]] WarmEditGpuPresentationPixelFormat pixel_format() const noexcept;
    [[nodiscard]] std::uint64_t resource_id() const noexcept;
    [[nodiscard]] std::uintptr_t native_texture_handle() const noexcept;
    [[nodiscard]] std::uintptr_t native_device_handle() const noexcept;

    // Shared storage is CPU-visible only for explicit fallback and contract validation. The span
    // includes row padding; consumers use row_stride_bytes() rather than assuming tight packing.
    [[nodiscard]] std::span<const std::uint8_t> rgba8_rows() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> materialize_packed_rgb8() const;

  private:
    struct Storage;
    explicit WarmEditGpuPresentationSurface(std::unique_ptr<Storage> storage);

    std::unique_ptr<Storage> storage_;

#if defined(__OBJC__)
    friend WarmEditGpuPresentationSurfacePreparation
    prepare_warm_edit_gpu_presentation_surface(id<MTLDevice> device, Dimensions dimensions);
    friend std::string encode_warm_edit_gpu_presentation_surface(
        id<MTLCommandBuffer> command_buffer,
        id<MTLBuffer> packed_rgb8,
        const WarmEditGpuPresentationSurface& surface
    );
#endif
};

struct WarmEditGpuPresentationSurfacePreparation final {
    std::shared_ptr<WarmEditGpuPresentationSurface> surface;
    std::string diagnostic;
};

// Process-local observability for the immutable presentation pipeline. Session preparation may
// call prewarm repeatedly, but the producer device is compiled exactly once, including a cached
// failed attempt.
[[nodiscard]] std::uint64_t warm_edit_gpu_presentation_pipeline_compile_attempt_count() noexcept;

#if defined(__OBJC__)
// Allocates one frame-owned surface. Preparation does not publish the surface and encoding does
// not commit the command buffer; the dispatcher publishes only after its existing completion,
// cancellation, and status checks have all succeeded.
[[nodiscard]] WarmEditGpuPresentationSurfacePreparation
prepare_warm_edit_gpu_presentation_surface(id<MTLDevice> device, Dimensions dimensions);

// Compile the tiny RGB8-to-RGBA presentation pipeline while the warm session itself is prepared,
// not on the first slider movement. Failure is cached and later becomes a named host fallback.
[[nodiscard]] std::string prewarm_warm_edit_gpu_presentation_surface(id<MTLDevice> device);

[[nodiscard]] std::string encode_warm_edit_gpu_presentation_surface(
    id<MTLCommandBuffer> command_buffer,
    id<MTLBuffer> packed_rgb8,
    const WarmEditGpuPresentationSurface& surface
);
#endif

} // namespace shadow::image::detail
