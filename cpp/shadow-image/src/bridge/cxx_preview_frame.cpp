#include <shadow/image/cxx_preview_frame.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace shadow::bridge {

InteractiveEditPreviewFrameHandle::InteractiveEditPreviewFrameHandle(
    image::EncodedProxy preview,
    std::optional<image::EditPreviewMaskCoverage> mask_coverage
)
    : frame_(std::move(preview), std::move(mask_coverage)) {}

InteractiveEditPreviewFrameHandle::InteractiveEditPreviewFrameHandle(
    image::InteractiveEditPreviewFrame frame
) : frame_(std::move(frame)) {}

InteractiveEditPreviewFrameHandle::~InteractiveEditPreviewFrameHandle() = default;

std::uint32_t InteractiveEditPreviewFrameHandle::width() const noexcept {
    return frame_.dimensions().width;
}

std::uint32_t InteractiveEditPreviewFrameHandle::height() const noexcept {
    return frame_.dimensions().height;
}

std::uint32_t
InteractiveEditPreviewFrameHandle::row_stride_bytes() const noexcept {
    return frame_.row_stride_bytes();
}

std::uint8_t InteractiveEditPreviewFrameHandle::storage_kind() const noexcept {
    return static_cast<std::uint8_t>(frame_.storage_kind());
}

std::uint32_t
InteractiveEditPreviewFrameHandle::native_texture_row_stride_bytes() const noexcept {
    return frame_.native_texture_row_stride_bytes();
}

std::uint8_t
InteractiveEditPreviewFrameHandle::native_texture_pixel_format() const noexcept {
    return frame_.native_texture_pixel_format();
}

std::uint64_t
InteractiveEditPreviewFrameHandle::native_resource_id() const noexcept {
    return frame_.native_resource_id();
}

std::size_t
InteractiveEditPreviewFrameHandle::native_texture_handle() const noexcept {
    return static_cast<std::size_t>(frame_.native_texture_handle());
}

std::size_t
InteractiveEditPreviewFrameHandle::native_device_handle() const noexcept {
    return static_cast<std::size_t>(frame_.native_device_handle());
}

std::size_t
InteractiveEditPreviewFrameHandle::materialized_pixel_bytes() const noexcept {
    return frame_.materialized_pixels().size();
}

std::size_t InteractiveEditPreviewFrameHandle::retained_bytes() const noexcept {
    return frame_.retained_bytes();
}

rust::String
InteractiveEditPreviewFrameHandle::presentation_fallback_diagnostic() const {
    return rust::String(frame_.presentation_fallback_diagnostic());
}

rust::Slice<const std::uint8_t>
InteractiveEditPreviewFrameHandle::materialize_pixels() const {
    const auto pixels = frame_.pixels();
    return {pixels.data(), pixels.size()};
}

bool InteractiveEditPreviewFrameHandle::mask_coverage_available() const noexcept {
    return frame_.mask_coverage() != nullptr;
}

rust::String InteractiveEditPreviewFrameHandle::mask_coverage_version() const {
    const auto* const coverage = frame_.mask_coverage();
    return coverage == nullptr ? rust::String{} : rust::String(coverage->version);
}

std::uint32_t
InteractiveEditPreviewFrameHandle::mask_coverage_layer_index() const noexcept {
    const auto* const coverage = frame_.mask_coverage();
    return coverage == nullptr ? 0U : coverage->layer_index;
}

std::uint32_t
InteractiveEditPreviewFrameHandle::mask_coverage_width() const noexcept {
    const auto* const coverage = frame_.mask_coverage();
    return coverage == nullptr ? 0U : coverage->dimensions.width;
}

std::uint32_t
InteractiveEditPreviewFrameHandle::mask_coverage_height() const noexcept {
    const auto* const coverage = frame_.mask_coverage();
    return coverage == nullptr ? 0U : coverage->dimensions.height;
}

std::uint32_t
InteractiveEditPreviewFrameHandle::mask_coverage_row_stride_bytes() const noexcept {
    const auto* const coverage = frame_.mask_coverage();
    return coverage == nullptr ? 0U : coverage->row_stride_bytes;
}

rust::Slice<const std::uint8_t>
InteractiveEditPreviewFrameHandle::mask_coverage_samples() const noexcept {
    const auto* const coverage = frame_.mask_coverage();
    if (coverage == nullptr) {
        return {};
    }
    return {coverage->samples.data(), coverage->samples.size()};
}

} // namespace shadow::bridge
