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

// All of LibRaw's user-visible processing switches are concentrated here rather than being
// spread across preview and detail render code. This is the configuration boundary between
// Shadow's provider-neutral decoder contract and LibRaw's private processing API. It deliberately
// describes the reference/development raster only; sensor-domain RAW white balance, DNG opcodes,
// optical profiles and camera-specific colour transforms will become separate pipeline stages.
//
// `demosaic_quality` maps directly to LibRaw's documented `user_qual` selector. It remains an
// implementation detail for now because the available algorithms vary with the linked LibRaw
// build. Shadow's UI will expose intent presets only after every provider can honour them.
struct LibRawDevelopmentSettings final {
    std::uint32_t schema_version = 1;
    bool use_camera_white_balance = true;
    bool use_camera_matrix = true;
    bool use_auto_brightness = false;
    bool use_exposure_correction = false;
    float brightness = 1.0F;
    float maximum_adjustment_threshold = 0.0F;
    std::uint16_t output_bits_per_channel = 16;
    std::int32_t demosaic_quality = 3;

    auto operator<=>(const LibRawDevelopmentSettings&) const = default;
};

inline constexpr std::uint32_t libraw_development_settings_schema_version = 1U;

[[nodiscard]] LibRawDevelopmentSettings default_libraw_development_settings() noexcept;
[[nodiscard]] std::string libraw_development_settings_signature(
    const LibRawDevelopmentSettings& settings
);

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
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    // Approximate focus distance in metres when a provider can establish it.  Zero means
    // unknown, never infinity or a guessed substitute. Lens vignetting calibration is
    // distance-dependent, so optical correction must leave that component disabled without it.
    double focus_distance_meters = 0.0;
    std::int64_t captured_at_unix_seconds = 0;
    std::string lens_make;
    std::string lens_model;
    double focal_length_35mm = 0.0;
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

// A renderer-produced record of the exact RAW-development request that yielded a processed
// reference raster. It intentionally lives beside the pixels, rather than in Recipe data: RAW
// provider behavior is source provenance, while a recipe is only the photographer's editable
// intent. A default/empty receipt means that a generic provider did not expose one; callers must
// not infer sensor-domain behavior from the absence of a receipt.
//
// `provider_version` is Shadow's complete cache-visible provider identity. `library_version`
// separately identifies the underlying renderer/library release when the provider has one; it
// may be empty for a provider that intentionally does not expose that implementation detail.
// `process_warnings` preserves a renderer's complete warning bit mask without collapsing future
// warning bits into a lossy boolean set.
inline constexpr std::uint32_t raw_development_receipt_schema_version = 1U;

struct RawDevelopmentReceipt final {
    // Zero means that the provider did not record RAW-development provenance for this buffer.
    std::uint32_t schema_version = 0U;
    std::string provider_id;
    std::string provider_version;
    std::string library_version;
    std::string development_settings_signature;
    std::uint32_t processed_linear_reference_contract_version = 0U;

    // These are LibRaw's declared pre-render image dimensions and orientation alongside the
    // actual output raster. They make a half-size or orientation-related difference auditable
    // without assuming that the metadata dimensions already describe the processed bitmap.
    Dimensions declared_image_dimensions;
    Dimensions rendered_dimensions;
    std::int32_t orientation = 0;
    bool half_size = false;

    bool use_camera_white_balance = false;
    bool use_camera_matrix = false;
    bool use_auto_brightness = false;
    bool use_exposure_correction = false;
    float brightness = 0.0F;
    float maximum_adjustment_threshold = 0.0F;
    std::uint16_t output_bits_per_channel = 0U;
    std::int32_t demosaic_quality = 0;
    std::int32_t output_color = 0;
    double gamma_inverse_power = 0.0;
    double gamma_linear_toe_slope = 0.0;

    // Opcode lengths describe the source declarations, not a claim that a particular LibRaw
    // build did or did not apply a stage. Consult process_warnings for the renderer's outcome.
    PendingCorrections declared_dng_opcode_lists;
    std::uint32_t process_warnings = 0U;

    [[nodiscard]] bool recorded() const noexcept {
        return schema_version != 0U;
    }

    // A caller may use `recorded()` to distinguish generic RGB input from provider-rendered RAW,
    // but must use this guard before interpreting the fields of a known schema. It keeps a future
    // private provider from being silently parsed as the current contract.
    [[nodiscard]] bool uses_current_schema() const noexcept {
        return schema_version == raw_development_receipt_schema_version;
    }
};

struct PixelBuffer final {
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::size_t row_stride_bytes = 0;
    RgbPrimaries primaries = RgbPrimaries::unknown;
    RgbTransferFunction transfer_function = RgbTransferFunction::unknown;
    RgbBufferReference reference = RgbBufferReference::unknown;
    std::vector<std::uint16_t> samples;
    RawDevelopmentReceipt raw_development_receipt;
};

// Version 1 fixes linear gamma, camera WB/matrix conversion, unit brightness, no exposure or
// histogram auto-brightening, and no frame-content adaptive maximum rescaling. Any change to
// those decode semantics must increment this cache-visible contract version.
inline constexpr std::uint32_t processed_linear_reference_rgb_contract_version = 1U;
inline constexpr float processed_linear_reference_maximum_adjustment_threshold = 0.0F;
// Version 4 accepts only processed linear sRGB/Rec.709-D65 input and first applies Shadow's
// neutral scene-to-display curve on luminance.  This creates a stable toe and shoulder for
// decoded RAW data before Oklab chroma is reduced at fixed mapped lightness and the sRGB OETF is
// applied. JPEG proxy encoding uses 4:4:4 sampling so this output contract does not discard
// chroma detail after scene-to-display rendering. It is a deterministic SDR display rendering,
// not a camera-JPEG emulation.
inline constexpr std::uint32_t display_srgb8_output_transform_version = 4U;
// The v4 gamut mapper is bounded work per out-of-gamut pixel. 0.5 is a conservative ceiling
// above the display-sRGB Oklab gamut; sixteen bisections resolve chroma well below one 8-bit code
// step.
inline constexpr double display_srgb8_maximum_oklab_chroma = 0.5;
inline constexpr std::uint32_t display_srgb8_gamut_search_iterations = 16U;

struct ProxyRequest final {
    std::uint32_t max_edge = 2'048;
    std::uint8_t jpeg_quality = 95;
};

struct EncodedProxy final {
    Dimensions dimensions;
    PreviewFormat format = PreviewFormat::jpeg;
    std::uint16_t bits_per_channel = 8;
    std::uint16_t channels = 3;
    std::vector<std::uint8_t> bytes;
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
    // Interactive preview is allowed to ask a provider for a bounded-quality reference. The
    // default keeps third-party/provider test implementations exact; LibRaw overrides it with
    // its documented half-size RAW path only when the native frame is far larger than the
    // requested preview. Full-detail rendering always calls render_reference_rgb().
    [[nodiscard]] virtual PixelBuffer render_reference_rgb_for_preview(
        std::uint32_t max_edge
    ) const {
        (void)max_edge;
        return render_reference_rgb();
    }
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

[[nodiscard]] std::unique_ptr<DecoderProvider> make_libraw_decoder_provider(
    LibRawDevelopmentSettings settings = default_libraw_development_settings()
);

[[nodiscard]] std::optional<std::size_t> select_best_preview(
    std::span<const PreviewDescriptor> previews
) noexcept;

[[nodiscard]] std::string_view to_string(PreviewFormat format) noexcept;

[[nodiscard]] Dimensions proxy_dimensions(Dimensions source, std::uint32_t max_edge);

[[nodiscard]] EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    ProxyRequest request = {}
);

} // namespace shadow::image
