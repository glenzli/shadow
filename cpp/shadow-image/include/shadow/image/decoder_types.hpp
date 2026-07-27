#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>

namespace shadow::image {

/// Shared raster dimensions used across decode, edit, and display contracts.
struct Dimensions final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] std::uint64_t pixel_count() const noexcept;
    auto operator<=>(const Dimensions&) const = default;
};

/// Insets from a stored raster to its active image rectangle.
struct Margins final {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;

    auto operator<=>(const Margins&) const = default;
};

enum class PreviewFormat : std::uint8_t {
    unknown,
    jpeg,
    bitmap,
    jpeg_xl,
    h265,
};

enum class ByteOrder : std::uint8_t {
    not_applicable,
    native,
    little_endian,
    big_endian,
};

// PixelBuffer names primaries, transfer, and processing reference separately. In particular,
// "sRGB primaries" must never be read as "sRGB-encoded samples".
enum class RgbPrimaries : std::uint8_t {
    unknown,
    srgb_rec709_d65,
};

enum class RgbTransferFunction : std::uint8_t {
    unknown,
    linear,
};

enum class RgbBufferReference : std::uint8_t {
    unknown,
    // RGB produced after a RAW provider's black subtraction, white balance, demosaic,
    // camera-to-output color conversion, and integer-range scaling. This is linear-light
    // processed RGB, not an untouched sensor-linear mosaic or a lossless radiance buffer.
    processed_raw,
    // RGB decoded from an ordinary rendered image (for example JPEG or SDR HEIF) after its
    // embedded profile, or an explicit sRGB fallback, has been transformed to linear
    // sRGB/Rec.709-D65. The source started display-referred, so it must not receive the RAW
    // scene-to-display curve at the output boundary. It nevertheless shares the same linear
    // node graph, history, LUT, cache and export pipeline as processed RAW RGB.
    decoded_raster,
};

struct PendingCorrections final {
    std::array<std::uint32_t, 3> dng_opcode_list_bytes{};

    [[nodiscard]] bool has_pending() const noexcept;
    auto operator<=>(const PendingCorrections&) const = default;
};

struct ProviderInfo final {
    std::string id;
    std::string version;
    bool dng_sdk = false;
    bool rawspeed = false;
    bool jpeg = false;
};

} // namespace shadow::image
