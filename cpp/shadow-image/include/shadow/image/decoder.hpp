#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

struct Dimensions final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] std::uint64_t pixel_count() const noexcept;
    auto operator<=>(const Dimensions&) const = default;
};

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

enum class ColorSpace : std::uint8_t {
    srgb,
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

struct DecodeCapabilities final {
    bool metadata = false;
    bool embedded_previews = false;
    bool mosaic = false;
    bool reference_rgb = false;
    PendingCorrections pending_corrections;
};

struct AssetMetadata final {
    std::string make;
    std::string model;
    std::string normalized_make;
    std::string normalized_model;
    std::string dng_version;
    std::uint32_t raw_count = 0;
    Dimensions raw_dimensions;
    Dimensions image_dimensions;
    Margins margins;
    std::int32_t orientation = 0;
    std::string cfa_pattern;
    std::uint32_t sensor_colors = 0;
    std::uint32_t sensor_bits = 0;
    std::uint32_t black_level = 0;
    std::uint32_t white_level = 0;
    std::array<double, 4> as_shot_neutral{};
    double baseline_exposure = 0.0;
};

struct PreviewDescriptor final {
    std::size_t id = 0;
    PreviewFormat format = PreviewFormat::unknown;
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::uint64_t encoded_bytes = 0;
    bool decodable = false;
};

struct PreviewPayload final {
    PreviewDescriptor descriptor;
    ByteOrder byte_order = ByteOrder::not_applicable;
    std::vector<std::uint8_t> bytes;
};

struct MosaicDescriptor final {
    Dimensions raw_dimensions;
    Dimensions image_dimensions;
    Margins margins;
    std::string cfa_pattern;
    std::uint32_t bits_per_sample = 0;
    std::uint32_t black_level = 0;
    std::uint32_t white_level = 0;
    std::size_t row_stride_bytes = 0;
    PendingCorrections pending_corrections;
};

struct MosaicBuffer final {
    MosaicDescriptor descriptor;
    std::vector<std::uint16_t> samples;
};

struct PixelBuffer final {
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::size_t row_stride_bytes = 0;
    ColorSpace color_space = ColorSpace::srgb;
    std::vector<std::uint16_t> samples;
};

enum class DecodeErrorCode : std::uint8_t {
    unsupported,
    io,
    corrupt_data,
    no_preview,
    unsupported_layout,
    invalid_request,
    resource_limit,
    cancelled,
    internal,
};

class DecodeError final : public std::runtime_error {
public:
    DecodeError(DecodeErrorCode code, int provider_code, std::string message);

    [[nodiscard]] DecodeErrorCode code() const noexcept;
    [[nodiscard]] int provider_code() const noexcept;

private:
    DecodeErrorCode code_;
    int provider_code_;
};

class DecodeSession {
public:
    DecodeSession() = default;
    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;
    DecodeSession(DecodeSession&&) = delete;
    DecodeSession& operator=(DecodeSession&&) = delete;
    virtual ~DecodeSession() = default;

    [[nodiscard]] virtual const AssetMetadata& metadata() const noexcept = 0;
    [[nodiscard]] virtual const DecodeCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::span<const PreviewDescriptor> previews() const noexcept = 0;
    [[nodiscard]] virtual PreviewPayload decode_preview(std::size_t id) = 0;
    [[nodiscard]] virtual MosaicBuffer decode_mosaic() = 0;
    [[nodiscard]] virtual PixelBuffer render_reference_rgb() const = 0;
};

class DecoderProvider {
public:
    DecoderProvider() = default;
    DecoderProvider(const DecoderProvider&) = delete;
    DecoderProvider& operator=(const DecoderProvider&) = delete;
    DecoderProvider(DecoderProvider&&) = delete;
    DecoderProvider& operator=(DecoderProvider&&) = delete;
    virtual ~DecoderProvider() = default;

    [[nodiscard]] virtual const ProviderInfo& info() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const = 0;
};

[[nodiscard]] std::unique_ptr<DecoderProvider> make_libraw_decoder_provider();

[[nodiscard]] std::optional<std::size_t> select_best_preview(
    std::span<const PreviewDescriptor> previews
) noexcept;

[[nodiscard]] std::string_view to_string(PreviewFormat format) noexcept;

} // namespace shadow::image
