#pragma once

#include "rust/cxx.h"

#include <shadow/image/edit_preview_frame.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace shadow::bridge {

// First language-boundary owner for an immutable interactive frame. CXX's UniquePtr support
// transfers this handle without copying its native vectors; every slice remains borrowed from
// this owner.
class InteractiveEditPreviewFrameHandle final {
public:
    InteractiveEditPreviewFrameHandle(
        image::EncodedProxy preview,
        std::optional<image::EditPreviewMaskCoverage> mask_coverage
    );
    explicit InteractiveEditPreviewFrameHandle(
        image::InteractiveEditPreviewFrame frame
    );
    ~InteractiveEditPreviewFrameHandle();

    InteractiveEditPreviewFrameHandle(const InteractiveEditPreviewFrameHandle&) = delete;
    InteractiveEditPreviewFrameHandle&
    operator=(const InteractiveEditPreviewFrameHandle&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint32_t row_stride_bytes() const noexcept;
    [[nodiscard]] std::uint8_t storage_kind() const noexcept;
    [[nodiscard]] std::uint32_t native_texture_row_stride_bytes() const noexcept;
    [[nodiscard]] std::uint8_t native_texture_pixel_format() const noexcept;
    [[nodiscard]] std::uint64_t native_resource_id() const noexcept;
    [[nodiscard]] std::size_t native_texture_handle() const noexcept;
    [[nodiscard]] std::size_t native_device_handle() const noexcept;
    [[nodiscard]] std::size_t materialized_pixel_bytes() const noexcept;
    [[nodiscard]] std::size_t retained_bytes() const noexcept;
    [[nodiscard]] rust::String presentation_fallback_diagnostic() const;
    [[nodiscard]] rust::Slice<const std::uint8_t> materialize_pixels() const;

    [[nodiscard]] bool mask_coverage_available() const noexcept;
    [[nodiscard]] rust::String mask_coverage_version() const;
    [[nodiscard]] std::uint32_t mask_coverage_layer_index() const noexcept;
    [[nodiscard]] bool mask_coverage_component_selected() const noexcept;
    [[nodiscard]] std::uint32_t mask_coverage_component_index() const noexcept;
    [[nodiscard]] std::uint32_t mask_coverage_width() const noexcept;
    [[nodiscard]] std::uint32_t mask_coverage_height() const noexcept;
    [[nodiscard]] std::uint32_t mask_coverage_row_stride_bytes() const noexcept;
    [[nodiscard]] rust::Slice<const std::uint8_t>
    mask_coverage_samples() const noexcept;

private:
    image::InteractiveEditPreviewFrame frame_;
};

} // namespace shadow::bridge
