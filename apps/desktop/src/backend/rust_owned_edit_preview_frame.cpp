#include "rust_owned_edit_preview_frame.hpp"

#include "../edit_mask_coverage_contract.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] std::uint64_t
checked_pixel_bytes(const std::uint32_t stride, const std::uint32_t height) {
    const auto bytes = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(height);
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::overflow_error("interactive preview byte count exceeds host limits");
    }
    return bytes;
}

class RustOwnedEditPreviewFrame final : public BackendEditPreviewFrame {
  public:
    explicit RustOwnedEditPreviewFrame(rust::Box<shadow::desktop::OwnedEditedPreview> owner) :
        owner_(std::move(owner)) {
        const auto& projection = owner_->projection();
        if (!owner_->interactive_frame_available()
            || projection.terminal != shadow::desktop::FfiEditPreviewTerminal::Completed
            || projection.width == 0U || projection.height == 0U
            || projection.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || projection.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument(
                "owned edit preview does not contain a valid interactive frame"
            );
        }

        const std::uint64_t expected_stride = static_cast<std::uint64_t>(projection.width) * 3U;
        if (expected_stride > std::numeric_limits<std::uint32_t>::max()
            || projection.row_stride_bytes != expected_stride || !projection.bytes.empty()) {
            throw std::invalid_argument("owned edit preview RGB8 descriptor is inconsistent");
        }
        const std::uint64_t expected_pixel_bytes =
            checked_pixel_bytes(projection.row_stride_bytes, projection.height);

        if (!projection.mask_coverage_available) {
            if (projection.mask_coverage_version != 0U
                || projection.mask_coverage_target_layer_index != 0U
                || projection.mask_selection_revision != 0U || projection.mask_coverage_width != 0U
                || projection.mask_coverage_height != 0U
                || projection.mask_coverage_row_stride_bytes != 0U
                || !owner_->interactive_mask_coverage_samples().empty()
                || !projection.mask_coverage_samples.empty()) {
                throw std::invalid_argument(
                    "unavailable owned mask coverage has non-empty sentinels"
                );
            }
        } else {
            const auto coverage = owner_->interactive_mask_coverage_samples();
            const std::uint64_t expected_coverage_bytes =
                static_cast<std::uint64_t>(projection.mask_coverage_width)
                * static_cast<std::uint64_t>(projection.mask_coverage_height);
            if (projection.mask_coverage_version != EDIT_MASK_COVERAGE_VERSION
                || projection.mask_coverage_width != projection.width
                || projection.mask_coverage_height != projection.height
                || projection.mask_coverage_row_stride_bytes != projection.mask_coverage_width
                || expected_coverage_bytes != static_cast<std::uint64_t>(coverage.size())
                || !projection.mask_coverage_samples.empty()) {
                throw std::invalid_argument("owned edit preview mask coverage is inconsistent");
            }
        }

        dimensions_ =
            QSize(static_cast<int>(projection.width), static_cast<int>(projection.height));
        row_stride_bytes_ = projection.row_stride_bytes;
        presentation_fallback_diagnostic_ =
            std::string(owner_->interactive_presentation_fallback_diagnostic());
        const std::uint64_t mask_bytes =
            static_cast<std::uint64_t>(owner_->interactive_mask_coverage_samples().size());
        const auto storage_kind = owner_->interactive_storage_kind();
        if (storage_kind == static_cast<std::uint8_t>(BackendEditPreviewStorage::HostRgb8)) {
            storage_kind_ = BackendEditPreviewStorage::HostRgb8;
            if (owner_->interactive_native_texture_row_stride_bytes() != 0U
                || owner_->interactive_native_texture_pixel_format() != 0U
                || owner_->interactive_native_resource_id() != 0U
                || owner_->interactive_native_texture_handle() != 0U
                || owner_->interactive_native_device_handle() != 0U
                || static_cast<std::uint64_t>(owner_->interactive_materialized_pixel_bytes())
                       != expected_pixel_bytes
                || static_cast<std::uint64_t>(owner_->interactive_retained_bytes())
                       != expected_pixel_bytes + mask_bytes) {
                throw std::invalid_argument("owned host edit preview descriptor is inconsistent");
            }
        } else if (storage_kind
                   == static_cast<std::uint8_t>(BackendEditPreviewStorage::AppleMetalRgba8Srgb)) {
            storage_kind_ = BackendEditPreviewStorage::AppleMetalRgba8Srgb;
            const auto native_row_stride = owner_->interactive_native_texture_row_stride_bytes();
            const std::uint64_t minimum_native_stride =
                static_cast<std::uint64_t>(projection.width) * 4U;
            const std::uint64_t native_bytes =
                checked_pixel_bytes(native_row_stride, projection.height);
            apple_metal_texture_ = BackendAppleMetalPreviewTexture{
                .resource_id = owner_->interactive_native_resource_id(),
                .texture_handle =
                    static_cast<std::uintptr_t>(owner_->interactive_native_texture_handle()),
                .device_handle =
                    static_cast<std::uintptr_t>(owner_->interactive_native_device_handle()),
                .row_stride_bytes = native_row_stride,
                .pixel_format = owner_->interactive_native_texture_pixel_format(),
            };
            if (static_cast<std::uint64_t>(native_row_stride) < minimum_native_stride
                || native_row_stride % 256U != 0U
                || apple_metal_texture_->pixel_format != BACKEND_APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT
                || apple_metal_texture_->resource_id == 0U
                || apple_metal_texture_->texture_handle == 0U
                || apple_metal_texture_->device_handle == 0U
                || owner_->interactive_materialized_pixel_bytes() != 0U
                || !presentation_fallback_diagnostic_.empty()
                || static_cast<std::uint64_t>(owner_->interactive_retained_bytes())
                       != native_bytes + mask_bytes) {
                throw std::invalid_argument("owned Metal edit preview descriptor is inconsistent");
            }
        } else {
            throw std::invalid_argument("owned edit preview uses an unknown storage kind");
        }
    }

    [[nodiscard]] QSize dimensions() const noexcept override {
        return dimensions_;
    }

    [[nodiscard]] std::size_t rowStrideBytes() const noexcept override {
        return row_stride_bytes_;
    }

    [[nodiscard]] BackendEditPreviewStorage storageKind() const noexcept override {
        return storage_kind_;
    }

    [[nodiscard]] std::optional<BackendAppleMetalPreviewTexture>
    appleMetalTexture() const noexcept override {
        return apple_metal_texture_;
    }

    [[nodiscard]] std::size_t materializedPixelBytes() const noexcept override {
        return owner_->interactive_materialized_pixel_bytes();
    }

    [[nodiscard]] std::span<const std::uint8_t> materializeRgb8() const override {
        const auto pixels = owner_->interactive_pixels();
        if (checked_pixel_bytes(
                static_cast<std::uint32_t>(row_stride_bytes_),
                static_cast<std::uint32_t>(dimensions_.height())
            )
            != static_cast<std::uint64_t>(pixels.size())) {
            throw std::runtime_error("materialized edit preview RGB8 byte count is inconsistent");
        }
        return {pixels.data(), pixels.size()};
    }

    [[nodiscard]] std::optional<BackendEditMaskCoverageView>
    maskCoverage() const noexcept override {
        const auto& projection = owner_->projection();
        if (!projection.mask_coverage_available) {
            return std::nullopt;
        }
        const auto samples = owner_->interactive_mask_coverage_samples();
        return BackendEditMaskCoverageView{
            .samples = {samples.data(), samples.size()},
            .version = projection.mask_coverage_version,
            .target_layer_index = projection.mask_coverage_target_layer_index,
            .selection_revision = projection.mask_selection_revision,
            .dimensions = dimensions_,
            .row_stride_bytes = projection.mask_coverage_row_stride_bytes,
        };
    }

    [[nodiscard]] std::string presentationFallbackDiagnostic() const override {
        return presentation_fallback_diagnostic_;
    }

    [[nodiscard]] std::uint64_t retainedBytes() const noexcept override {
        return static_cast<std::uint64_t>(owner_->interactive_retained_bytes());
    }

  private:
    rust::Box<shadow::desktop::OwnedEditedPreview> owner_;
    QSize dimensions_;
    std::size_t row_stride_bytes_ = 0;
    BackendEditPreviewStorage storage_kind_ = BackendEditPreviewStorage::HostRgb8;
    std::optional<BackendAppleMetalPreviewTexture> apple_metal_texture_;
    std::string presentation_fallback_diagnostic_;
};

} // namespace

std::shared_ptr<const BackendEditPreviewFrame>
makeRustOwnedEditPreviewFrame(rust::Box<shadow::desktop::OwnedEditedPreview> owner) {
    return std::make_shared<RustOwnedEditPreviewFrame>(std::move(owner));
}
