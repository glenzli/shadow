#include <shadow/image/edit_preview_frame.hpp>

#include <shadow/image/decoder_error.hpp>

#include "warm_edit_gpu_presentation_surface.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace shadow::image {

namespace {

[[nodiscard]] std::uint32_t packed_rgb8_stride(const Dimensions dimensions) {
    const auto stride = static_cast<std::uint64_t>(dimensions.width) * 3U;
    if (dimensions.width == 0U || dimensions.height == 0U
        || stride > std::numeric_limits<std::uint32_t>::max()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "interactive preview frame has invalid RGB8 dimensions"
        );
    }
    return static_cast<std::uint32_t>(stride);
}

[[nodiscard]] std::size_t
packed_byte_count(const std::uint32_t row_stride_bytes, const std::uint32_t height) {
    const auto bytes =
        static_cast<std::uint64_t>(row_stride_bytes) * static_cast<std::uint64_t>(height);
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "interactive preview frame exceeds the host address space"
        );
    }
    return static_cast<std::size_t>(bytes);
}

} // namespace

struct InteractiveEditPreviewFrame::Storage final {
    Dimensions dimensions;
    std::uint32_t row_stride_bytes = 0U;
    InteractiveEditPreviewStorageKind storage_kind = InteractiveEditPreviewStorageKind::host_rgb8;
    std::shared_ptr<const detail::WarmEditGpuPresentationSurface> presentation_surface;
    mutable std::once_flag materialization_once;
    mutable std::vector<std::uint8_t> pixels;
    mutable std::atomic<bool> pixels_materialized{false};
    std::string presentation_fallback_diagnostic;
    std::optional<EditPreviewMaskCoverage> mask_coverage;
};

bool EditPreviewMaskCoverage::valid() const noexcept {
    return version == edit_preview_mask_coverage_version && dimensions.width > 0U
           && dimensions.height > 0U && row_stride_bytes == dimensions.width
           && dimensions.pixel_count() == samples.size();
}

InteractiveEditPreviewFrame::InteractiveEditPreviewFrame(
    EncodedProxy preview,
    std::optional<EditPreviewMaskCoverage> mask_coverage,
    std::string presentation_fallback_diagnostic
) {
    const auto row_stride_bytes = packed_rgb8_stride(preview.dimensions);
    if (preview.format != PreviewFormat::bitmap || preview.bits_per_channel != 8U
        || preview.channels != 3U
        || preview.bytes.size() != packed_byte_count(row_stride_bytes, preview.dimensions.height)) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "interactive preview frame requires tightly packed display RGB8 pixels"
        );
    }
    if (mask_coverage.has_value()
        && (!mask_coverage->valid() || mask_coverage->dimensions != preview.dimensions)) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "interactive preview frame has invalid or unpaired mask coverage"
        );
    }

    auto storage = std::make_unique<Storage>();
    storage->dimensions = preview.dimensions;
    storage->row_stride_bytes = row_stride_bytes;
    storage->storage_kind = InteractiveEditPreviewStorageKind::host_rgb8;
    storage->pixels = std::move(preview.bytes);
    storage->pixels_materialized.store(true, std::memory_order_relaxed);
    storage->presentation_fallback_diagnostic = std::move(presentation_fallback_diagnostic);
    storage->mask_coverage = std::move(mask_coverage);
    storage_ = std::move(storage);
}

InteractiveEditPreviewFrame::InteractiveEditPreviewFrame(
    const Dimensions dimensions,
    std::shared_ptr<const detail::WarmEditGpuPresentationSurface> presentation_surface,
    std::optional<EditPreviewMaskCoverage> mask_coverage
) {
    const auto row_stride_bytes = packed_rgb8_stride(dimensions);
    if (!presentation_surface || presentation_surface->dimensions() != dimensions
        || static_cast<std::uint64_t>(presentation_surface->row_stride_bytes())
               < static_cast<std::uint64_t>(dimensions.width) * 4U
        || presentation_surface->pixel_format()
               != detail::WarmEditGpuPresentationPixelFormat::rgba8_unorm_display_srgb
        || presentation_surface->resource_id() == 0U
        || presentation_surface->native_texture_handle() == 0U
        || presentation_surface->native_device_handle() == 0U) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "interactive preview frame received an invalid Metal presentation surface"
        );
    }
    if (mask_coverage.has_value()
        && (!mask_coverage->valid() || mask_coverage->dimensions != dimensions)) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "interactive preview frame has invalid or unpaired mask coverage"
        );
    }

    auto storage = std::make_unique<Storage>();
    storage->dimensions = dimensions;
    storage->row_stride_bytes = row_stride_bytes;
    storage->storage_kind = InteractiveEditPreviewStorageKind::apple_metal_rgba8_srgb;
    storage->presentation_surface = std::move(presentation_surface);
    storage->mask_coverage = std::move(mask_coverage);
    storage_ = std::move(storage);
}

InteractiveEditPreviewFrame::~InteractiveEditPreviewFrame() = default;
InteractiveEditPreviewFrame::InteractiveEditPreviewFrame(InteractiveEditPreviewFrame&&) noexcept =
    default;
InteractiveEditPreviewFrame&
InteractiveEditPreviewFrame::operator=(InteractiveEditPreviewFrame&&) noexcept = default;

Dimensions InteractiveEditPreviewFrame::dimensions() const noexcept {
    return storage_->dimensions;
}

std::uint32_t InteractiveEditPreviewFrame::row_stride_bytes() const noexcept {
    return storage_->row_stride_bytes;
}

InteractiveEditPreviewStorageKind InteractiveEditPreviewFrame::storage_kind() const noexcept {
    return storage_->storage_kind;
}

std::uint32_t InteractiveEditPreviewFrame::native_texture_row_stride_bytes() const noexcept {
    return storage_->presentation_surface ? storage_->presentation_surface->row_stride_bytes() : 0U;
}

std::uint8_t InteractiveEditPreviewFrame::native_texture_pixel_format() const noexcept {
    return storage_->presentation_surface
               ? static_cast<std::uint8_t>(storage_->presentation_surface->pixel_format())
               : 0U;
}

std::uint64_t InteractiveEditPreviewFrame::native_resource_id() const noexcept {
    return storage_->presentation_surface ? storage_->presentation_surface->resource_id() : 0U;
}

std::uintptr_t InteractiveEditPreviewFrame::native_texture_handle() const noexcept {
    return storage_->presentation_surface ? storage_->presentation_surface->native_texture_handle()
                                          : 0U;
}

std::uintptr_t InteractiveEditPreviewFrame::native_device_handle() const noexcept {
    return storage_->presentation_surface ? storage_->presentation_surface->native_device_handle()
                                          : 0U;
}

std::span<const std::uint8_t> InteractiveEditPreviewFrame::materialized_pixels() const noexcept {
    if (!storage_->pixels_materialized.load(std::memory_order_acquire)) {
        return {};
    }
    return storage_->pixels;
}

std::span<const std::uint8_t> InteractiveEditPreviewFrame::pixels() const {
    if (storage_->presentation_surface) {
        std::call_once(storage_->materialization_once, [this]() {
            auto pixels = storage_->presentation_surface->materialize_packed_rgb8();
            if (pixels.size()
                != packed_byte_count(storage_->row_stride_bytes, storage_->dimensions.height)) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Metal presentation surface could not materialize packed RGB8"
                );
            }
            storage_->pixels = std::move(pixels);
            storage_->pixels_materialized.store(true, std::memory_order_release);
        });
    }
    return storage_->pixels;
}

std::size_t InteractiveEditPreviewFrame::retained_bytes() const noexcept {
    std::size_t retained = storage_->pixels_materialized.load(std::memory_order_acquire)
                               ? storage_->pixels.size()
                               : 0U;
    if (storage_->presentation_surface) {
        retained += storage_->presentation_surface->rgba8_rows().size();
    }
    if (storage_->mask_coverage) {
        retained += storage_->mask_coverage->samples.size();
    }
    return retained;
}

const std::string& InteractiveEditPreviewFrame::presentation_fallback_diagnostic() const noexcept {
    return storage_->presentation_fallback_diagnostic;
}

const EditPreviewMaskCoverage* InteractiveEditPreviewFrame::mask_coverage() const noexcept {
    return storage_->mask_coverage.has_value() ? &*storage_->mask_coverage : nullptr;
}

} // namespace shadow::image
