#pragma once

#include <QSize>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// Read-only ownership boundary for one interactive preview publication.
//
// Implementations retain the actual payload owner. Store snapshots and texture
// factories share this interface so neither RGB8 nor paired R8 coverage needs
// to be materialized again while crossing the desktop shell.
struct BackendEditMaskCoverageView final {
    std::span<const std::uint8_t> samples;
    std::uint32_t version = 0;
    std::uint32_t target_layer_index = 0;
    std::uint64_t selection_revision = 0;
    QSize dimensions;
    std::size_t row_stride_bytes = 0;
};

enum class BackendEditPreviewStorage : std::uint8_t {
    HostRgb8 = 0U,
    AppleMetalRgba8Srgb = 1U,
};

// Semantic byte contract: the RGBA values are display-sRGB encoded. The
// native Metal texture uses an unorm physical view so Qt Quick samples those
// presentation-ready bytes with the same appearance as the settled preview.
inline constexpr std::uint8_t BACKEND_APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT = 1U;

struct BackendAppleMetalPreviewTexture final {
    std::uint64_t resource_id = 0U;
    std::uintptr_t texture_handle = 0U;
    std::uintptr_t device_handle = 0U;
    std::size_t row_stride_bytes = 0U;
    std::uint8_t pixel_format = 0U;
};

class BackendEditPreviewFrame {
  public:
    virtual ~BackendEditPreviewFrame() = default;

    [[nodiscard]] virtual QSize dimensions() const noexcept = 0;
    // Logical packed RGB8 fallback stride. Native RGBA storage publishes its
    // independently aligned row stride through appleMetalTexture().
    [[nodiscard]] virtual std::size_t rowStrideBytes() const noexcept = 0;
    [[nodiscard]] virtual BackendEditPreviewStorage storageKind() const noexcept = 0;
    [[nodiscard]] virtual std::optional<BackendAppleMetalPreviewTexture>
    appleMetalTexture() const noexcept = 0;
    [[nodiscard]] virtual std::size_t materializedPixelBytes() const noexcept = 0;
    // Explicit compatibility fallback. Native texture inspection and mask
    // coverage must never call this potentially throwing materializer.
    [[nodiscard]] virtual std::span<const std::uint8_t> materializeRgb8() const = 0;
    [[nodiscard]] virtual std::optional<BackendEditMaskCoverageView>
    maskCoverage() const noexcept = 0;
    [[nodiscard]] virtual std::string presentationFallbackDiagnostic() const = 0;
    // Materialization can add a retained packed RGB allocation, so this is a
    // live query rather than a constructor-cached descriptor.
    [[nodiscard]] virtual std::uint64_t retainedBytes() const noexcept = 0;
};
