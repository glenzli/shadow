#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace shadow::image {

// This product includes DNG technology under license by Adobe.
//
// Camera profiles are deliberately separate from SourceProfileCatalog. A DCP
// describes camera-native colorimetry and must be resolved after RawFrame
// development; a source profile describes an already-standardized RGB
// fallback. Keeping the contracts separate prevents an exact DCP and a generic
// base curve from being applied to the same source accidentally.
inline constexpr std::uint32_t dcp_profile_schema_version = 1U;
inline constexpr std::uint32_t dcp_parse_receipt_schema_version = 1U;

enum class DcpByteOrder : std::uint8_t {
    little_endian,
    big_endian,
};

enum class DcpEmbedPolicy : std::uint32_t {
    allow_copying_dng_only = 0U,
    embed_if_used = 1U,
    embed_never = 2U,
    no_restrictions = 3U,
};

enum class DcpDefaultBlackRender : std::uint32_t {
    automatic = 0U,
    none = 1U,
};

enum class DcpTableEncoding : std::uint32_t {
    linear = 0U,
    srgb = 1U,
};

struct DcpMatrix3x3 final {
    // DNG matrices are serialized in row-scan order. Transform code must make
    // its row/column-vector convention explicit rather than transposing this
    // storage implicitly.
    std::array<double, 9> row_major{};

    auto operator<=>(const DcpMatrix3x3&) const = default;
};

struct DcpToneCurvePoint final {
    float input = 0.0F;
    float output = 0.0F;

    auto operator<=>(const DcpToneCurvePoint&) const = default;
};

struct DcpHsvDelta final {
    float hue_shift_degrees = 0.0F;
    float saturation_scale = 1.0F;
    float value_scale = 1.0F;

    auto operator<=>(const DcpHsvDelta&) const = default;
};

struct DcpHsvTable final {
    std::uint32_t hue_divisions = 0U;
    std::uint32_t saturation_divisions = 0U;
    std::uint32_t value_divisions = 0U;
    DcpTableEncoding encoding = DcpTableEncoding::linear;
    // DNG order is value outermost, hue next, saturation innermost:
    // ((value * hue_divisions) + hue) * saturation_divisions + saturation.
    std::vector<DcpHsvDelta> entries;

    auto operator<=>(const DcpHsvTable&) const = default;
};

struct DcpIlluminantCalibration final {
    // EXIF LightSource value. Zero is the single-illuminant default "unknown";
    // custom illuminant value 255 is deliberately outside DCP v1.
    std::uint16_t illuminant = 0U;
    bool illuminant_was_explicit = false;
    DcpMatrix3x3 color_matrix;
    // The second calibration may legally inherit ColorMatrix1 when only an
    // alternate illuminant/table is supplied. This bit keeps that provenance
    // cache- and receipt-visible.
    bool color_matrix_was_inherited = false;
    std::optional<DcpMatrix3x3> forward_matrix;
    std::optional<DcpHsvTable> hue_sat_map;

    auto operator<=>(const DcpIlluminantCalibration&) const = default;
};

struct DcpParseReceipt final {
    std::uint32_t schema_version = dcp_parse_receipt_schema_version;
    DcpByteOrder byte_order = DcpByteOrder::little_endian;
    std::uint64_t source_bytes = 0U;
    std::uint32_t first_ifd_offset = 0U;
    std::uint16_t ifd_entry_count = 0U;

    auto operator<=>(const DcpParseReceipt&) const = default;
};

struct DcpProfile final {
    std::uint32_t schema_version = dcp_profile_schema_version;
    DcpParseReceipt parse_receipt;
    std::string unique_camera_model;
    std::string profile_name;
    std::string profile_copyright;
    std::string profile_calibration_signature;
    DcpEmbedPolicy embed_policy = DcpEmbedPolicy::allow_copying_dng_only;
    bool embed_policy_was_explicit = false;
    DcpDefaultBlackRender default_black_render = DcpDefaultBlackRender::automatic;
    bool default_black_render_was_explicit = false;
    DcpIlluminantCalibration calibration1;
    std::optional<DcpIlluminantCalibration> calibration2;
    std::optional<double> baseline_exposure_offset_ev;
    std::vector<DcpToneCurvePoint> tone_curve;
    std::optional<DcpHsvTable> look_table;

    auto operator<=>(const DcpProfile&) const = default;
};

enum class DcpParseErrorCode : std::uint8_t {
    empty_document,
    document_too_large,
    truncated_document,
    invalid_byte_order,
    invalid_magic,
    invalid_ifd,
    too_many_ifd_entries,
    multiple_ifds,
    duplicate_or_unsorted_tag,
    invalid_tiff_type,
    invalid_tag_type,
    invalid_tag_count,
    invalid_tag_value,
    missing_required_tag,
    inconsistent_profile,
    unsupported_profile_feature,
    file_io,
};

class DcpParseError final : public std::invalid_argument {
public:
    DcpParseError(
        DcpParseErrorCode code,
        std::string message,
        std::uint64_t byte_offset = 0U,
        std::optional<std::uint16_t> tag = std::nullopt
    );

    [[nodiscard]] DcpParseErrorCode code() const noexcept;
    [[nodiscard]] std::uint64_t byte_offset() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> tag() const noexcept;

private:
    DcpParseErrorCode code_;
    std::uint64_t byte_offset_;
    std::optional<std::uint16_t> tag_;
};

// Parses one standalone DCP camera-profile IFD. It accepts standard TIFF scalar
// storage in either byte order but requires the DCP "CR" magic, one bounded
// IFD, exact types/counts for every supported tag, and the three-color-plane
// SDR subset represented above. Unknown optional TIFF tags remain forward
// compatible; known profile features that would change rendering but are not
// represented by DCP v1 fail closed.
[[nodiscard]] DcpProfile parse_dcp_profile(std::span<const std::byte> bytes);

// The file overload applies the same size bound before allocating. Paths are
// not retained in the parsed profile; a future CameraProfileCatalog owns
// filesystem identity and content hashing.
[[nodiscard]] DcpProfile load_dcp_profile(const std::filesystem::path& path);

} // namespace shadow::image
