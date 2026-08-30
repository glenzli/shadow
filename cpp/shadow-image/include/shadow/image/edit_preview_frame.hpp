#pragma once

#include <shadow/image/proxy_rendering.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

namespace detail {
class WarmEditGpuPresentationSurface;
}

inline constexpr std::string_view edit_preview_mask_coverage_version =
    "shadow.edit-preview-mask-coverage.v2:r8-pre-adjustment-input:layer-or-component:paired-geometry";

// Optional transient selection evidence paired with one completed preview frame. Coverage belongs
// to one authored layer, is evaluated against that layer's pre-adjustment input, includes mask
// inversion but not layer opacity, and receives the same crop/orientation geometry as the RGB8
// frame. It never enters the durable JPEG or cache identity.
struct EditPreviewMaskCoverage final {
    std::string version;
    std::uint32_t layer_index = 0U;
    std::optional<std::uint32_t> component_index;
    Dimensions dimensions;
    std::uint32_t row_stride_bytes = 0U;
    std::vector<std::uint8_t> samples;

    [[nodiscard]] bool valid() const noexcept;
};

enum class InteractiveEditPreviewStorageKind : std::uint8_t {
    host_rgb8 = 0U,
    apple_metal_rgba8_srgb = 1U,
};

// Immutable ownership boundary for one interactive display frame. Construction moves either the
// renderer's packed RGB8 or one completed native presentation surface plus optional R8 coverage
// into one owner and validates their paired descriptors exactly once. Explicit host materialization
// remains address-stable even when this owner is moved.
class InteractiveEditPreviewFrame final {
public:
    InteractiveEditPreviewFrame(
        EncodedProxy preview,
        std::optional<EditPreviewMaskCoverage> mask_coverage,
        std::string presentation_fallback_diagnostic = {}
    );
    ~InteractiveEditPreviewFrame();

    InteractiveEditPreviewFrame(const InteractiveEditPreviewFrame&) = delete;
    InteractiveEditPreviewFrame& operator=(const InteractiveEditPreviewFrame&) = delete;
    InteractiveEditPreviewFrame(InteractiveEditPreviewFrame&&) noexcept;
    InteractiveEditPreviewFrame& operator=(InteractiveEditPreviewFrame&&) noexcept;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t row_stride_bytes() const noexcept;
    [[nodiscard]] InteractiveEditPreviewStorageKind storage_kind() const noexcept;
    [[nodiscard]] std::uint32_t native_texture_row_stride_bytes() const noexcept;
    [[nodiscard]] std::uint8_t native_texture_pixel_format() const noexcept;
    [[nodiscard]] std::uint64_t native_resource_id() const noexcept;
    [[nodiscard]] std::uintptr_t native_texture_handle() const noexcept;
    [[nodiscard]] std::uintptr_t native_device_handle() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> materialized_pixels() const noexcept;
    // Explicit compatibility fallback. Native-texture construction and descriptor inspection do
    // not call this method; its packed RGB8 allocation is initialized at most once and remains
    // address-stable for the rest of the frame lifetime.
    [[nodiscard]] std::span<const std::uint8_t> pixels() const;
    [[nodiscard]] std::size_t retained_bytes() const noexcept;
    [[nodiscard]] const std::string&
    presentation_fallback_diagnostic() const noexcept;
    [[nodiscard]] const EditPreviewMaskCoverage* mask_coverage() const noexcept;

private:
    InteractiveEditPreviewFrame(
        Dimensions dimensions,
        std::shared_ptr<const detail::WarmEditGpuPresentationSurface>
            presentation_surface,
        std::optional<EditPreviewMaskCoverage> mask_coverage
    );

    struct Storage;
    std::unique_ptr<const Storage> storage_;

    friend class WarmEditPreviewSession;
};

} // namespace shadow::image
