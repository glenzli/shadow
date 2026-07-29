#include "warm_edit_gpu_presentation_surface.hpp"

#include <utility>

namespace shadow::image::detail {

struct WarmEditGpuPresentationSurface::Storage final {
    Dimensions dimensions;
};

WarmEditGpuPresentationSurface::WarmEditGpuPresentationSurface(std::unique_ptr<Storage> storage) :
    storage_(std::move(storage)) {}

WarmEditGpuPresentationSurface::~WarmEditGpuPresentationSurface() = default;

Dimensions WarmEditGpuPresentationSurface::dimensions() const noexcept {
    return storage_ ? storage_->dimensions : Dimensions{};
}

std::uint32_t WarmEditGpuPresentationSurface::row_stride_bytes() const noexcept {
    return 0U;
}

WarmEditGpuPresentationPixelFormat WarmEditGpuPresentationSurface::pixel_format() const noexcept {
    return WarmEditGpuPresentationPixelFormat::rgba8_unorm_display_srgb;
}

std::uint64_t WarmEditGpuPresentationSurface::resource_id() const noexcept {
    return 0U;
}

std::uintptr_t WarmEditGpuPresentationSurface::native_texture_handle() const noexcept {
    return 0U;
}

std::uintptr_t WarmEditGpuPresentationSurface::native_device_handle() const noexcept {
    return 0U;
}

std::span<const std::uint8_t> WarmEditGpuPresentationSurface::rgba8_rows() const noexcept {
    return {};
}

std::vector<std::uint8_t> WarmEditGpuPresentationSurface::materialize_packed_rgb8() const {
    return {};
}

std::uint64_t warm_edit_gpu_presentation_pipeline_compile_attempt_count() noexcept {
    return 0U;
}

} // namespace shadow::image::detail
