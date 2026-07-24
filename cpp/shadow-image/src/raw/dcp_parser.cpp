#include <shadow/image/camera_profile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::size_t dcp_header_bytes = 8U;
inline constexpr std::size_t maximum_dcp_document_bytes = 32U * 1'024U * 1'024U;
inline constexpr std::uint16_t maximum_ifd_entries = 1'024U;
inline constexpr std::size_t maximum_profile_string_bytes = 4'096U;
// Some public camera profiles use a dense 8,192-point identity/base curve.
// Keep a finite decoded-memory bound without rejecting those profiles.
inline constexpr std::size_t maximum_tone_curve_points = 65'536U;
inline constexpr std::size_t maximum_hsv_table_entries = 1'048'576U;

enum class TiffType : std::uint16_t {
    byte = 1U,
    ascii = 2U,
    short_value = 3U,
    long_value = 4U,
    rational = 5U,
    signed_byte = 6U,
    undefined = 7U,
    signed_short = 8U,
    signed_long = 9U,
    signed_rational = 10U,
    float_value = 11U,
    double_value = 12U,
};

enum class DcpTag : std::uint16_t {
    unique_camera_model = 50'708U,
    color_matrix1 = 50'721U,
    color_matrix2 = 50'722U,
    camera_calibration1 = 50'723U,
    camera_calibration2 = 50'724U,
    reduction_matrix1 = 50'725U,
    reduction_matrix2 = 50'726U,
    analog_balance = 50'727U,
    calibration_illuminant1 = 50'778U,
    calibration_illuminant2 = 50'779U,
    profile_calibration_signature = 50'932U,
    extra_camera_profiles = 50'933U,
    profile_name = 50'936U,
    profile_hue_sat_map_dims = 50'937U,
    profile_hue_sat_map_data1 = 50'938U,
    profile_hue_sat_map_data2 = 50'939U,
    profile_tone_curve = 50'940U,
    profile_embed_policy = 50'941U,
    profile_copyright = 50'942U,
    forward_matrix1 = 50'964U,
    forward_matrix2 = 50'965U,
    profile_look_table_dims = 50'981U,
    profile_look_table_data = 50'982U,
    profile_hue_sat_map_encoding = 51'107U,
    profile_look_table_encoding = 51'108U,
    baseline_exposure_offset = 51'109U,
    default_black_render = 51'110U,
    profile_gain_table_map = 52'525U,
    calibration_illuminant3 = 52'529U,
    camera_calibration3 = 52'530U,
    color_matrix3 = 52'531U,
    forward_matrix3 = 52'532U,
    illuminant_data1 = 52'533U,
    illuminant_data2 = 52'534U,
    illuminant_data3 = 52'535U,
    profile_hue_sat_map_data3 = 52'537U,
    reduction_matrix3 = 52'538U,
    rgb_tables = 52'543U,
    profile_gain_table_map2 = 52'544U,
    profile_dynamic_range = 52'551U,
};

struct TiffEntry final {
    std::uint16_t tag = 0U;
    TiffType type = TiffType::undefined;
    std::uint32_t count = 0U;
    std::size_t entry_offset = 0U;
    std::size_t payload_offset = 0U;
    std::size_t payload_bytes = 0U;
    bool inline_payload = false;
};

[[noreturn]] void fail(
    const DcpParseErrorCode code,
    const std::string_view message,
    const std::size_t offset = 0U,
    const std::optional<std::uint16_t> tag = std::nullopt
) {
    throw DcpParseError(code, std::string(message), offset, tag);
}

[[nodiscard]] std::size_t checked_add(
    const std::size_t left,
    const std::size_t right,
    const DcpParseErrorCode code,
    const std::string_view message,
    const std::size_t offset = 0U,
    const std::optional<std::uint16_t> tag = std::nullopt
) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        fail(code, message, offset, tag);
    }
    return left + right;
}

[[nodiscard]] std::size_t checked_multiply(
    const std::size_t left,
    const std::size_t right,
    const DcpParseErrorCode code,
    const std::string_view message,
    const std::size_t offset = 0U,
    const std::optional<std::uint16_t> tag = std::nullopt
) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        fail(code, message, offset, tag);
    }
    return left * right;
}

[[nodiscard]] std::size_t tiff_type_bytes(
    const TiffType type,
    const std::size_t offset,
    const std::uint16_t tag
) {
    switch (type) {
    case TiffType::byte:
    case TiffType::ascii:
    case TiffType::signed_byte:
    case TiffType::undefined:
        return 1U;
    case TiffType::short_value:
    case TiffType::signed_short:
        return 2U;
    case TiffType::long_value:
    case TiffType::signed_long:
    case TiffType::float_value:
        return 4U;
    case TiffType::rational:
    case TiffType::signed_rational:
    case TiffType::double_value:
        return 8U;
    }
    fail(
        DcpParseErrorCode::invalid_tiff_type,
        "DCP IFD entry uses an unsupported TIFF scalar type",
        offset,
        tag
    );
}

class BoundedReader final {
public:
    BoundedReader(const std::span<const std::byte> bytes, const DcpByteOrder byte_order)
        : bytes_(bytes), byte_order_(byte_order) {
    }

    [[nodiscard]] std::uint16_t u16(
        const std::size_t offset,
        const std::optional<std::uint16_t> tag = std::nullopt
    ) const {
        require_range(offset, 2U, tag);
        const auto first = byte(offset);
        const auto second = byte(offset + 1U);
        if (byte_order_ == DcpByteOrder::little_endian) {
            return static_cast<std::uint16_t>(first | (second << 8U));
        }
        return static_cast<std::uint16_t>((first << 8U) | second);
    }

    [[nodiscard]] std::uint32_t u32(
        const std::size_t offset,
        const std::optional<std::uint16_t> tag = std::nullopt
    ) const {
        require_range(offset, 4U, tag);
        std::uint32_t result = 0U;
        if (byte_order_ == DcpByteOrder::little_endian) {
            for (std::size_t index = 0U; index < 4U; ++index) {
                result |= byte(offset + index) << (index * 8U);
            }
        } else {
            for (std::size_t index = 0U; index < 4U; ++index) {
                result = (result << 8U) | byte(offset + index);
            }
        }
        return result;
    }

    [[nodiscard]] std::int32_t i32(
        const std::size_t offset,
        const std::optional<std::uint16_t> tag = std::nullopt
    ) const {
        return std::bit_cast<std::int32_t>(u32(offset, tag));
    }

    [[nodiscard]] float f32(
        const std::size_t offset,
        const std::optional<std::uint16_t> tag = std::nullopt
    ) const {
        return std::bit_cast<float>(u32(offset, tag));
    }

    [[nodiscard]] std::span<const std::byte> span(
        const std::size_t offset,
        const std::size_t size,
        const std::optional<std::uint16_t> tag = std::nullopt
    ) const {
        require_range(offset, size, tag);
        return bytes_.subspan(offset, size);
    }

    void require_range(
        const std::size_t offset,
        const std::size_t size,
        const std::optional<std::uint16_t> tag = std::nullopt
    ) const {
        if (offset > bytes_.size() || size > bytes_.size() - offset) {
            fail(
                DcpParseErrorCode::truncated_document,
                "DCP payload extends beyond the bounded document",
                offset,
                tag
            );
        }
    }

private:
    [[nodiscard]] std::uint32_t byte(const std::size_t offset) const {
        return static_cast<std::uint8_t>(bytes_[offset]);
    }

    std::span<const std::byte> bytes_;
    DcpByteOrder byte_order_;
};

[[nodiscard]] bool ranges_overlap(
    const std::size_t first_offset,
    const std::size_t first_size,
    const std::size_t second_offset,
    const std::size_t second_size
) noexcept {
    return first_offset < second_offset + second_size
        && second_offset < first_offset + first_size;
}

[[nodiscard]] std::vector<TiffEntry> parse_ifd(
    const BoundedReader& reader,
    const std::size_t ifd_offset,
    DcpParseReceipt& receipt
) {
    reader.require_range(ifd_offset, 2U);
    const std::uint16_t entry_count = reader.u16(ifd_offset);
    if (entry_count > maximum_ifd_entries) {
        fail(
            DcpParseErrorCode::too_many_ifd_entries,
            "DCP IFD entry count exceeds the v1 safety bound",
            ifd_offset
        );
    }
    const std::size_t entries_bytes = checked_multiply(
        entry_count,
        12U,
        DcpParseErrorCode::invalid_ifd,
        "DCP IFD entry table size overflows",
        ifd_offset
    );
    const std::size_t ifd_bytes = checked_add(
        checked_add(
            2U,
            entries_bytes,
            DcpParseErrorCode::invalid_ifd,
            "DCP IFD table size overflows",
            ifd_offset
        ),
        4U,
        DcpParseErrorCode::invalid_ifd,
        "DCP IFD next-offset field overflows",
        ifd_offset
    );
    reader.require_range(ifd_offset, ifd_bytes);
    receipt.ifd_entry_count = entry_count;

    std::vector<TiffEntry> entries;
    entries.reserve(entry_count);
    std::optional<std::uint16_t> previous_tag;
    for (std::size_t index = 0U; index < entry_count; ++index) {
        const std::size_t entry_offset = ifd_offset + 2U + index * 12U;
        const std::uint16_t tag = reader.u16(entry_offset);
        if (previous_tag.has_value() && tag <= *previous_tag) {
            fail(
                DcpParseErrorCode::duplicate_or_unsorted_tag,
                "DCP IFD tags must be strictly increasing and unique",
                entry_offset,
                tag
            );
        }
        previous_tag = tag;
        const auto type_number = reader.u16(entry_offset + 2U, tag);
        if (type_number < static_cast<std::uint16_t>(TiffType::byte)
            || type_number > static_cast<std::uint16_t>(TiffType::double_value)) {
            fail(
                DcpParseErrorCode::invalid_tiff_type,
                "DCP IFD entry uses an unsupported TIFF scalar type",
                entry_offset + 2U,
                tag
            );
        }
        const auto type = static_cast<TiffType>(type_number);
        const std::uint32_t count = reader.u32(entry_offset + 4U, tag);
        if (count == 0U) {
            fail(
                DcpParseErrorCode::invalid_tag_count,
                "DCP IFD entries must contain at least one value",
                entry_offset + 4U,
                tag
            );
        }
        const std::size_t payload_bytes = checked_multiply(
            count,
            tiff_type_bytes(type, entry_offset + 2U, tag),
            DcpParseErrorCode::invalid_tag_count,
            "DCP tag payload size overflows",
            entry_offset + 4U,
            tag
        );
        const bool inline_payload = payload_bytes <= 4U;
        const std::size_t payload_offset = inline_payload
            ? entry_offset + 8U
            : static_cast<std::size_t>(reader.u32(entry_offset + 8U, tag));
        if (!inline_payload && payload_offset < dcp_header_bytes) {
            fail(
                DcpParseErrorCode::invalid_ifd,
                "DCP out-of-line payload points into its file header",
                entry_offset + 8U,
                tag
            );
        }
        reader.require_range(payload_offset, payload_bytes, tag);
        if (!inline_payload
            && ranges_overlap(payload_offset, payload_bytes, ifd_offset, ifd_bytes)) {
            fail(
                DcpParseErrorCode::invalid_ifd,
                "DCP out-of-line payload overlaps its IFD structure",
                payload_offset,
                tag
            );
        }
        entries.push_back(TiffEntry{
            .tag = tag,
            .type = type,
            .count = count,
            .entry_offset = entry_offset,
            .payload_offset = payload_offset,
            .payload_bytes = payload_bytes,
            .inline_payload = inline_payload,
        });
    }

    const std::size_t next_ifd_offset = ifd_offset + 2U + entries_bytes;
    if (reader.u32(next_ifd_offset) != 0U) {
        fail(
            DcpParseErrorCode::multiple_ifds,
            "standalone DCP v1 accepts exactly one camera-profile IFD",
            next_ifd_offset
        );
    }
    return entries;
}

[[nodiscard]] const TiffEntry* find_entry(
    const std::vector<TiffEntry>& entries,
    const DcpTag tag
) noexcept {
    const auto numeric_tag = static_cast<std::uint16_t>(tag);
    const auto iterator = std::lower_bound(
        entries.begin(),
        entries.end(),
        numeric_tag,
        [](const TiffEntry& entry, const std::uint16_t value) {
            return entry.tag < value;
        }
    );
    return iterator != entries.end() && iterator->tag == numeric_tag
        ? &*iterator
        : nullptr;
}

[[nodiscard]] bool type_is(
    const TiffType type,
    const std::initializer_list<TiffType> expected
) noexcept {
    return std::find(expected.begin(), expected.end(), type) != expected.end();
}

void require_type(
    const TiffEntry& entry,
    const std::initializer_list<TiffType> expected
) {
    if (!type_is(entry.type, expected)) {
        fail(
            DcpParseErrorCode::invalid_tag_type,
            "DCP tag uses a TIFF type not permitted by the DNG specification",
            entry.entry_offset + 2U,
            entry.tag
        );
    }
}

void require_count(const TiffEntry& entry, const std::uint32_t expected) {
    if (entry.count != expected) {
        fail(
            DcpParseErrorCode::invalid_tag_count,
            "DCP tag has a value count not permitted by the DNG specification",
            entry.entry_offset + 4U,
            entry.tag
        );
    }
}

[[nodiscard]] bool valid_utf8(const std::span<const std::byte> text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0U;
        std::uint32_t codepoint = 0U;
        if ((lead & 0xE0U) == 0xC0U) {
            continuation_count = 1U;
            codepoint = lead & 0x1FU;
            if (codepoint < 2U) {
                return false;
            }
        } else if ((lead & 0xF0U) == 0xE0U) {
            continuation_count = 2U;
            codepoint = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            continuation_count = 3U;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t continuation = 0U; continuation < continuation_count; ++continuation) {
            const auto byte = static_cast<std::uint8_t>(text[index + continuation + 1U]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((continuation_count == 2U && codepoint < 0x800U)
            || (continuation_count == 3U && codepoint < 0x1'0000U)
            || codepoint > 0x10'FFFFU
            || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] std::string read_string(
    const BoundedReader& reader,
    const TiffEntry& entry,
    const bool ascii_only,
    const bool require_nonempty
) {
    require_type(
        entry,
        ascii_only
            ? std::initializer_list<TiffType>{TiffType::ascii}
            : std::initializer_list<TiffType>{TiffType::ascii, TiffType::byte}
    );
    if (entry.count > maximum_profile_string_bytes) {
        fail(
            DcpParseErrorCode::invalid_tag_count,
            "DCP profile string exceeds the v1 safety bound",
            entry.entry_offset + 4U,
            entry.tag
        );
    }
    const auto payload = reader.span(entry.payload_offset, entry.payload_bytes, entry.tag);
    if (static_cast<std::uint8_t>(payload.back()) != 0U) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP profile strings must include one trailing null byte",
            entry.payload_offset + entry.payload_bytes - 1U,
            entry.tag
        );
    }
    const auto text = payload.first(payload.size() - 1U);
    if (require_nonempty && text.empty()) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "required DCP profile string is empty",
            entry.payload_offset,
            entry.tag
        );
    }
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const auto byte = static_cast<std::uint8_t>(text[index]);
        if (byte == 0U || byte < 0x20U || byte == 0x7FU || (ascii_only && byte > 0x7FU)) {
            fail(
                DcpParseErrorCode::invalid_tag_value,
                "DCP profile string contains an embedded null or control byte",
                entry.payload_offset + index,
                entry.tag
            );
        }
    }
    if (!ascii_only && !valid_utf8(text)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP BYTE string is not valid UTF-8",
            entry.payload_offset,
            entry.tag
        );
    }
    return std::string(
        reinterpret_cast<const char*>(text.data()),
        text.size()
    );
}

[[nodiscard]] double read_rational(
    const BoundedReader& reader,
    const TiffEntry& entry,
    const std::size_t index
) {
    const std::size_t offset = entry.payload_offset + index * 8U;
    double value = 0.0;
    if (entry.type == TiffType::rational) {
        const auto numerator = reader.u32(offset, entry.tag);
        const auto denominator = reader.u32(offset + 4U, entry.tag);
        if (denominator == 0U) {
            fail(
                DcpParseErrorCode::invalid_tag_value,
                "DCP rational denominator is zero",
                offset + 4U,
                entry.tag
            );
        }
        value = static_cast<double>(numerator) / static_cast<double>(denominator);
    } else {
        const auto numerator = reader.i32(offset, entry.tag);
        const auto denominator = reader.i32(offset + 4U, entry.tag);
        if (denominator == 0) {
            fail(
                DcpParseErrorCode::invalid_tag_value,
                "DCP signed-rational denominator is zero",
                offset + 4U,
                entry.tag
            );
        }
        value = static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    if (!std::isfinite(value)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP rational does not evaluate to a finite value",
            offset,
            entry.tag
        );
    }
    return value;
}

[[nodiscard]] double matrix_determinant(const DcpMatrix3x3& matrix) noexcept {
    const auto& m = matrix.row_major;
    return m[0] * (m[4] * m[8] - m[5] * m[7])
        - m[1] * (m[3] * m[8] - m[5] * m[6])
        + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

[[nodiscard]] DcpMatrix3x3 read_matrix(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::signed_rational});
    require_count(entry, 9U);
    DcpMatrix3x3 matrix;
    for (std::size_t index = 0U; index < matrix.row_major.size(); ++index) {
        matrix.row_major[index] = read_rational(reader, entry, index);
    }
    const double determinant = matrix_determinant(matrix);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP color matrix is singular or numerically unusable",
            entry.payload_offset,
            entry.tag
        );
    }
    return matrix;
}

[[nodiscard]] bool legal_light_source(const std::uint16_t value) noexcept {
    return value <= 4U || (value >= 9U && value <= 24U) || value == 255U;
}

[[nodiscard]] std::uint16_t read_illuminant(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::short_value});
    require_count(entry, 1U);
    const std::uint16_t illuminant = reader.u16(entry.payload_offset, entry.tag);
    if (!legal_light_source(illuminant)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP calibration illuminant is not a legal EXIF LightSource value",
            entry.payload_offset,
            entry.tag
        );
    }
    if (illuminant == 255U) {
        fail(
            DcpParseErrorCode::unsupported_profile_feature,
            "DCP v1 does not support custom calibration illuminant data",
            entry.payload_offset,
            entry.tag
        );
    }
    return illuminant;
}

[[nodiscard]] std::array<std::uint32_t, 3> read_table_dimensions(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::long_value});
    require_count(entry, 3U);
    std::array<std::uint32_t, 3> dimensions{};
    for (std::size_t index = 0U; index < dimensions.size(); ++index) {
        dimensions[index] = reader.u32(entry.payload_offset + index * 4U, entry.tag);
    }
    if (dimensions[0] < 1U || dimensions[1] < 2U || dimensions[2] < 1U) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP HSV table dimensions require H>=1, S>=2, and V>=1",
            entry.payload_offset,
            entry.tag
        );
    }
    const std::size_t hue_saturation = checked_multiply(
        dimensions[0],
        dimensions[1],
        DcpParseErrorCode::invalid_tag_value,
        "DCP HSV table dimensions overflow",
        entry.payload_offset,
        entry.tag
    );
    const std::size_t entries = checked_multiply(
        hue_saturation,
        dimensions[2],
        DcpParseErrorCode::invalid_tag_value,
        "DCP HSV table dimensions overflow",
        entry.payload_offset,
        entry.tag
    );
    if (entries > maximum_hsv_table_entries) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP HSV table exceeds the v1 decoded-entry safety bound",
            entry.payload_offset,
            entry.tag
        );
    }
    return dimensions;
}

[[nodiscard]] DcpTableEncoding read_table_encoding(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::long_value});
    require_count(entry, 1U);
    const std::uint32_t value = reader.u32(entry.payload_offset, entry.tag);
    if (value > static_cast<std::uint32_t>(DcpTableEncoding::srgb)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP HSV table encoding is neither linear nor sRGB",
            entry.payload_offset,
            entry.tag
        );
    }
    return static_cast<DcpTableEncoding>(value);
}

[[nodiscard]] DcpHsvTable read_hsv_table(
    const BoundedReader& reader,
    const TiffEntry& data_entry,
    const std::array<std::uint32_t, 3>& dimensions,
    const DcpTableEncoding encoding
) {
    require_type(data_entry, {TiffType::float_value});
    const std::size_t table_entries = static_cast<std::size_t>(dimensions[0])
        * static_cast<std::size_t>(dimensions[1])
        * static_cast<std::size_t>(dimensions[2]);
    const std::size_t expected_values = table_entries * 3U;
    if (data_entry.count != expected_values) {
        fail(
            DcpParseErrorCode::invalid_tag_count,
            "DCP HSV table data count does not match its dimensions",
            data_entry.entry_offset + 4U,
            data_entry.tag
        );
    }
    if (dimensions[2] == 1U && encoding != DcpTableEncoding::linear) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP sRGB table encoding is not applicable to a 2.5D table",
            data_entry.entry_offset,
            data_entry.tag
        );
    }

    DcpHsvTable table{
        .hue_divisions = dimensions[0],
        .saturation_divisions = dimensions[1],
        .value_divisions = dimensions[2],
        .encoding = encoding,
    };
    table.entries.reserve(table_entries);
    for (std::size_t index = 0U; index < table_entries; ++index) {
        const std::size_t base = data_entry.payload_offset + index * 12U;
        const float hue_shift = reader.f32(base, data_entry.tag);
        const float saturation_scale = reader.f32(base + 4U, data_entry.tag);
        const float value_scale = reader.f32(base + 8U, data_entry.tag);
        if (!std::isfinite(hue_shift) || !std::isfinite(saturation_scale)
            || !std::isfinite(value_scale)) {
            fail(
                DcpParseErrorCode::invalid_tag_value,
                "DCP HSV table contains a non-finite delta",
                base,
                data_entry.tag
            );
        }
        const std::size_t saturation_index =
            index % static_cast<std::size_t>(dimensions[1]);
        if (saturation_index == 0U && value_scale != 1.0F) {
            fail(
                DcpParseErrorCode::invalid_tag_value,
                "DCP zero-saturation table entries require a value scale of exactly 1",
                base + 8U,
                data_entry.tag
            );
        }
        table.entries.push_back(DcpHsvDelta{
            .hue_shift_degrees = hue_shift,
            .saturation_scale = saturation_scale,
            .value_scale = value_scale,
        });
    }
    return table;
}

[[nodiscard]] std::vector<DcpToneCurvePoint> read_tone_curve(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::float_value});
    if (entry.count < 4U || entry.count % 2U != 0U
        || entry.count / 2U > maximum_tone_curve_points) {
        fail(
            DcpParseErrorCode::invalid_tag_count,
            "SDR DCP tone curve requires 2 through 65536 coordinate pairs",
            entry.entry_offset + 4U,
            entry.tag
        );
    }
    std::vector<DcpToneCurvePoint> points;
    points.reserve(entry.count / 2U);
    for (std::size_t index = 0U; index < entry.count / 2U; ++index) {
        const std::size_t offset = entry.payload_offset + index * 8U;
        const float input = reader.f32(offset, entry.tag);
        const float output = reader.f32(offset + 4U, entry.tag);
        if (!std::isfinite(input) || !std::isfinite(output)
            || input < 0.0F || input > 1.0F || output < 0.0F || output > 1.0F
            || (!points.empty() && input <= points.back().input)) {
            fail(
                DcpParseErrorCode::invalid_tag_value,
                "SDR DCP tone curve points must be finite, normalized, and strictly ordered by input",
                offset,
                entry.tag
            );
        }
        points.push_back(DcpToneCurvePoint{.input = input, .output = output});
    }
    if (points.front().input != 0.0F || points.front().output != 0.0F
        || points.back().input != 1.0F || points.back().output != 1.0F) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "SDR DCP tone curve endpoints must be exactly (0,0) and (1,1)",
            entry.payload_offset,
            entry.tag
        );
    }
    return points;
}

void reject_unsupported_known_features(const std::vector<TiffEntry>& entries) {
    for (const DcpTag tag : {
             DcpTag::camera_calibration1,
             DcpTag::camera_calibration2,
             DcpTag::reduction_matrix1,
             DcpTag::reduction_matrix2,
             DcpTag::analog_balance,
             DcpTag::extra_camera_profiles,
             DcpTag::profile_gain_table_map,
             DcpTag::calibration_illuminant3,
             DcpTag::camera_calibration3,
             DcpTag::color_matrix3,
             DcpTag::forward_matrix3,
             DcpTag::illuminant_data1,
             DcpTag::illuminant_data2,
             DcpTag::illuminant_data3,
             DcpTag::profile_hue_sat_map_data3,
             DcpTag::reduction_matrix3,
             DcpTag::rgb_tables,
             DcpTag::profile_gain_table_map2,
         }) {
        if (const auto* entry = find_entry(entries, tag); entry != nullptr) {
            fail(
                DcpParseErrorCode::unsupported_profile_feature,
                "DCP contains a known color-profile feature outside the three-plane SDR v1 contract",
                entry->entry_offset,
                entry->tag
            );
        }
    }
}

void validate_dynamic_range(
    const BoundedReader& reader,
    const std::vector<TiffEntry>& entries
) {
    const auto* entry = find_entry(entries, DcpTag::profile_dynamic_range);
    if (entry == nullptr) {
        return;
    }
    require_type(*entry, {TiffType::undefined});
    require_count(*entry, 8U);
    const std::uint16_t version = reader.u16(entry->payload_offset, entry->tag);
    const std::uint16_t dynamic_range =
        reader.u16(entry->payload_offset + 2U, entry->tag);
    const float hint_max_output = reader.f32(entry->payload_offset + 4U, entry->tag);
    if (version != 1U || dynamic_range > 1U || !std::isfinite(hint_max_output)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP ProfileDynamicRange contains an invalid version or value",
            entry->payload_offset,
            entry->tag
        );
    }
    if (dynamic_range == 1U) {
        fail(
            DcpParseErrorCode::unsupported_profile_feature,
            "DCP v1 accepts SDR profiles only",
            entry->payload_offset + 2U,
            entry->tag
        );
    }
    if (hint_max_output > 1.0F) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "SDR DCP ProfileDynamicRange hint may not exceed 1",
            entry->payload_offset + 4U,
            entry->tag
        );
    }
}

[[nodiscard]] DcpEmbedPolicy read_embed_policy(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::long_value});
    require_count(entry, 1U);
    const auto policy = reader.u32(entry.payload_offset, entry.tag);
    if (policy > static_cast<std::uint32_t>(DcpEmbedPolicy::no_restrictions)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP ProfileEmbedPolicy must be in the range 0 through 3",
            entry.payload_offset,
            entry.tag
        );
    }
    return static_cast<DcpEmbedPolicy>(policy);
}

[[nodiscard]] DcpDefaultBlackRender read_black_render(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::long_value});
    require_count(entry, 1U);
    const auto value = reader.u32(entry.payload_offset, entry.tag);
    if (value > static_cast<std::uint32_t>(DcpDefaultBlackRender::none)) {
        fail(
            DcpParseErrorCode::invalid_tag_value,
            "DCP DefaultBlackRender must be Auto or None",
            entry.payload_offset,
            entry.tag
        );
    }
    return static_cast<DcpDefaultBlackRender>(value);
}

[[nodiscard]] double read_baseline_exposure_offset(
    const BoundedReader& reader,
    const TiffEntry& entry
) {
    require_type(entry, {TiffType::rational, TiffType::signed_rational});
    require_count(entry, 1U);
    return read_rational(reader, entry, 0U);
}

} // namespace

DcpParseError::DcpParseError(
    const DcpParseErrorCode code,
    std::string message,
    const std::uint64_t byte_offset,
    const std::optional<std::uint16_t> tag
)
    : std::invalid_argument(std::move(message)),
      code_(code),
      byte_offset_(byte_offset),
      tag_(tag) {
}

DcpParseErrorCode DcpParseError::code() const noexcept {
    return code_;
}

std::uint64_t DcpParseError::byte_offset() const noexcept {
    return byte_offset_;
}

std::optional<std::uint16_t> DcpParseError::tag() const noexcept {
    return tag_;
}

DcpProfile parse_dcp_profile(const std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        fail(DcpParseErrorCode::empty_document, "DCP document is empty");
    }
    if (bytes.size() > maximum_dcp_document_bytes) {
        fail(
            DcpParseErrorCode::document_too_large,
            "DCP document exceeds the 32 MiB safety bound"
        );
    }
    if (bytes.size() < dcp_header_bytes) {
        fail(
            DcpParseErrorCode::truncated_document,
            "DCP document is shorter than its eight-byte header"
        );
    }

    DcpByteOrder byte_order = DcpByteOrder::little_endian;
    const auto first = static_cast<std::uint8_t>(bytes[0]);
    const auto second = static_cast<std::uint8_t>(bytes[1]);
    if (first == static_cast<std::uint8_t>('I')
        && second == static_cast<std::uint8_t>('I')) {
        byte_order = DcpByteOrder::little_endian;
    } else if (first == static_cast<std::uint8_t>('M')
               && second == static_cast<std::uint8_t>('M')) {
        byte_order = DcpByteOrder::big_endian;
    } else {
        fail(
            DcpParseErrorCode::invalid_byte_order,
            "DCP byte-order marker must be II or MM"
        );
    }

    const BoundedReader reader(bytes, byte_order);
    if (reader.u16(2U) != 0x4352U) {
        fail(
            DcpParseErrorCode::invalid_magic,
            "standalone camera profile must use the DCP CR magic",
            2U
        );
    }
    const std::uint32_t ifd_offset = reader.u32(4U);
    if (ifd_offset < dcp_header_bytes) {
        fail(
            DcpParseErrorCode::invalid_ifd,
            "DCP first IFD offset points into its file header",
            4U
        );
    }

    DcpProfile profile;
    profile.parse_receipt = DcpParseReceipt{
        .schema_version = dcp_parse_receipt_schema_version,
        .byte_order = byte_order,
        .source_bytes = bytes.size(),
        .first_ifd_offset = ifd_offset,
    };
    const auto entries = parse_ifd(reader, ifd_offset, profile.parse_receipt);
    reject_unsupported_known_features(entries);
    validate_dynamic_range(reader, entries);

    const auto* unique_camera_model = find_entry(entries, DcpTag::unique_camera_model);
    const auto* color_matrix1 = find_entry(entries, DcpTag::color_matrix1);
    if (unique_camera_model == nullptr || color_matrix1 == nullptr) {
        fail(
            DcpParseErrorCode::missing_required_tag,
            "DCP v1 requires UniqueCameraModel and ColorMatrix1"
        );
    }
    profile.unique_camera_model = read_string(
        reader,
        *unique_camera_model,
        true,
        true
    );
    profile.calibration1.color_matrix = read_matrix(reader, *color_matrix1);

    if (const auto* entry = find_entry(entries, DcpTag::profile_name); entry != nullptr) {
        profile.profile_name = read_string(reader, *entry, false, false);
    }
    if (const auto* entry = find_entry(entries, DcpTag::profile_copyright);
        entry != nullptr) {
        profile.profile_copyright = read_string(reader, *entry, false, false);
    }
    if (const auto* entry = find_entry(entries, DcpTag::profile_calibration_signature);
        entry != nullptr) {
        profile.profile_calibration_signature = read_string(reader, *entry, false, false);
    }
    if (const auto* entry = find_entry(entries, DcpTag::profile_embed_policy);
        entry != nullptr) {
        profile.embed_policy = read_embed_policy(reader, *entry);
        profile.embed_policy_was_explicit = true;
    }
    if (const auto* entry = find_entry(entries, DcpTag::default_black_render);
        entry != nullptr) {
        profile.default_black_render = read_black_render(reader, *entry);
        profile.default_black_render_was_explicit = true;
    }
    if (const auto* entry = find_entry(entries, DcpTag::baseline_exposure_offset);
        entry != nullptr) {
        profile.baseline_exposure_offset_ev =
            read_baseline_exposure_offset(reader, *entry);
    }
    if (const auto* entry = find_entry(entries, DcpTag::profile_tone_curve);
        entry != nullptr) {
        profile.tone_curve = read_tone_curve(reader, *entry);
    }

    if (const auto* entry = find_entry(entries, DcpTag::calibration_illuminant1);
        entry != nullptr) {
        profile.calibration1.illuminant = read_illuminant(reader, *entry);
        profile.calibration1.illuminant_was_explicit = true;
    }
    if (const auto* entry = find_entry(entries, DcpTag::forward_matrix1);
        entry != nullptr) {
        profile.calibration1.forward_matrix = read_matrix(reader, *entry);
    }

    const auto* color_matrix2 = find_entry(entries, DcpTag::color_matrix2);
    const auto* illuminant2 = find_entry(entries, DcpTag::calibration_illuminant2);
    const auto* forward_matrix2 = find_entry(entries, DcpTag::forward_matrix2);
    const auto* hue_sat_data2 = find_entry(entries, DcpTag::profile_hue_sat_map_data2);
    const bool has_second_calibration =
        color_matrix2 != nullptr || illuminant2 != nullptr
        || forward_matrix2 != nullptr || hue_sat_data2 != nullptr;
    if (has_second_calibration && illuminant2 == nullptr) {
        fail(
            DcpParseErrorCode::inconsistent_profile,
            "DCP second calibration data requires CalibrationIlluminant2"
        );
    }
    if (has_second_calibration) {
        DcpIlluminantCalibration second;
        second.illuminant = read_illuminant(reader, *illuminant2);
        second.illuminant_was_explicit = true;
        if (profile.calibration1.illuminant == 0U || second.illuminant == 0U) {
            fail(
                DcpParseErrorCode::inconsistent_profile,
                "dual-illuminant DCP profiles may not use unknown illuminants"
            );
        }
        if (color_matrix2 != nullptr) {
            second.color_matrix = read_matrix(reader, *color_matrix2);
        } else {
            second.color_matrix = profile.calibration1.color_matrix;
            second.color_matrix_was_inherited = true;
        }
        if (forward_matrix2 != nullptr) {
            second.forward_matrix = read_matrix(reader, *forward_matrix2);
        }
        profile.calibration2 = std::move(second);
    }

    const auto* hue_sat_dimensions =
        find_entry(entries, DcpTag::profile_hue_sat_map_dims);
    const auto* hue_sat_data1 =
        find_entry(entries, DcpTag::profile_hue_sat_map_data1);
    const auto* hue_sat_encoding =
        find_entry(entries, DcpTag::profile_hue_sat_map_encoding);
    if ((hue_sat_dimensions == nullptr) != (hue_sat_data1 == nullptr)) {
        fail(
            DcpParseErrorCode::inconsistent_profile,
            "DCP HueSatMap dimensions and Data1 must be present together"
        );
    }
    if (hue_sat_data2 != nullptr && hue_sat_dimensions == nullptr) {
        fail(
            DcpParseErrorCode::inconsistent_profile,
            "DCP HueSatMap Data2 requires dimensions and Data1"
        );
    }
    // Some public camera-profile generators write a harmless encoding tag
    // without the corresponding optional table. Validate the scalar even when
    // it has no table to affect, but do not reject an otherwise usable profile.
    const auto parsed_hue_sat_encoding = hue_sat_encoding == nullptr
        ? DcpTableEncoding::linear
        : read_table_encoding(reader, *hue_sat_encoding);
    if (hue_sat_dimensions != nullptr) {
        const auto dimensions = read_table_dimensions(reader, *hue_sat_dimensions);
        profile.calibration1.hue_sat_map =
            read_hsv_table(reader, *hue_sat_data1, dimensions, parsed_hue_sat_encoding);
        if (hue_sat_data2 != nullptr) {
            if (!profile.calibration2.has_value()) {
                fail(
                    DcpParseErrorCode::inconsistent_profile,
                    "DCP HueSatMap Data2 requires a second illuminant"
                );
            }
            profile.calibration2->hue_sat_map =
                read_hsv_table(reader, *hue_sat_data2, dimensions, parsed_hue_sat_encoding);
        }
    }

    const auto* look_dimensions =
        find_entry(entries, DcpTag::profile_look_table_dims);
    const auto* look_data = find_entry(entries, DcpTag::profile_look_table_data);
    const auto* look_encoding =
        find_entry(entries, DcpTag::profile_look_table_encoding);
    if ((look_dimensions == nullptr) != (look_data == nullptr)) {
        fail(
            DcpParseErrorCode::inconsistent_profile,
            "DCP LookTable dimensions and data must be present together"
        );
    }
    const auto parsed_look_encoding = look_encoding == nullptr
        ? DcpTableEncoding::linear
        : read_table_encoding(reader, *look_encoding);
    if (look_dimensions != nullptr) {
        const auto dimensions = read_table_dimensions(reader, *look_dimensions);
        profile.look_table =
            read_hsv_table(reader, *look_data, dimensions, parsed_look_encoding);
    }
    return profile;
}

DcpProfile load_dcp_profile(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw DcpParseError(
            DcpParseErrorCode::file_io,
            "could not inspect DCP profile file: " + path.string()
        );
    }
    if (file_size == 0U) {
        throw DcpParseError(DcpParseErrorCode::empty_document, "DCP profile file is empty");
    }
    if (file_size > maximum_dcp_document_bytes
        || file_size > std::numeric_limits<std::size_t>::max()) {
        throw DcpParseError(
            DcpParseErrorCode::document_too_large,
            "DCP profile file exceeds the 32 MiB safety bound"
        );
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw DcpParseError(
            DcpParseErrorCode::file_io,
            "could not open DCP profile file: " + path.string()
        );
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw DcpParseError(
            DcpParseErrorCode::file_io,
            "could not read the complete DCP profile file: " + path.string()
        );
    }
    return parse_dcp_profile(bytes);
}

} // namespace shadow::image
