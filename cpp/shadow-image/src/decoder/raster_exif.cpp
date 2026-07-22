#include "raster_exif.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace shadow::image {

namespace {

[[nodiscard]] bool in_bounds(
    const std::size_t offset,
    const std::size_t length,
    const std::size_t available
) noexcept {
    return offset <= available && length <= available - offset;
}

[[nodiscard]] std::uint16_t read_u16(
    const std::uint8_t* bytes,
    const bool little_endian
) noexcept {
    if (little_endian) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0])
            | (static_cast<std::uint16_t>(bytes[1]) << 8U)
        );
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U)
        | static_cast<std::uint16_t>(bytes[1])
    );
}

[[nodiscard]] std::uint32_t read_u32(
    const std::uint8_t* bytes,
    const bool little_endian
) noexcept {
    if (little_endian) {
        return static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8U)
            | (static_cast<std::uint32_t>(bytes[2]) << 16U)
            | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }
    return (static_cast<std::uint32_t>(bytes[0]) << 24U)
        | (static_cast<std::uint32_t>(bytes[1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[2]) << 8U)
        | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::size_t tiff_type_size(const std::uint16_t type) noexcept {
    switch (type) {
    case 1U: // BYTE
    case 2U: // ASCII
    case 7U: // UNDEFINED
        return 1U;
    case 3U: // SHORT
        return 2U;
    case 4U: // LONG
        return 4U;
    case 5U: // RATIONAL
        return 8U;
    default:
        return 0U;
    }
}

struct TiffValue final {
    const std::uint8_t* bytes = nullptr;
    std::size_t byte_count = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
};

[[nodiscard]] std::optional<TiffValue> tiff_value(
    const std::uint8_t* tiff,
    const std::size_t bytes,
    const bool little_endian,
    const std::size_t entry_offset
) noexcept {
    if (!in_bounds(entry_offset, 12U, bytes)) {
        return std::nullopt;
    }
    const std::uint16_t type = read_u16(tiff + entry_offset + 2U, little_endian);
    const std::uint32_t count = read_u32(tiff + entry_offset + 4U, little_endian);
    const std::size_t unit = tiff_type_size(type);
    if (unit == 0U || count == 0U || count > std::numeric_limits<std::size_t>::max() / unit) {
        return std::nullopt;
    }
    const std::size_t value_bytes = static_cast<std::size_t>(count) * unit;
    if (value_bytes <= 4U) {
        return TiffValue{tiff + entry_offset + 8U, value_bytes, type, count};
    }
    const std::size_t value_offset = read_u32(tiff + entry_offset + 8U, little_endian);
    if (!in_bounds(value_offset, value_bytes, bytes)) {
        return std::nullopt;
    }
    return TiffValue{tiff + value_offset, value_bytes, type, count};
}

void copy_ascii(char* const destination, const std::size_t capacity, const TiffValue& value) {
    if (capacity == 0U || value.type != 2U || value.bytes == nullptr) {
        return;
    }
    const std::size_t source_length = std::min(value.byte_count, capacity - 1U);
    std::size_t output_length = 0U;
    for (; output_length < source_length && value.bytes[output_length] != 0U; ++output_length) {
        const std::uint8_t byte = value.bytes[output_length];
        destination[output_length] = byte >= 0x20U && byte <= 0x7eU
            ? static_cast<char>(byte)
            : ' ';
    }
    destination[output_length] = '\0';
}

[[nodiscard]] std::optional<double> tiff_scalar(
    const TiffValue& value,
    const bool little_endian
) noexcept {
    if (value.bytes == nullptr || value.count == 0U) {
        return std::nullopt;
    }
    switch (value.type) {
    case 3U:
        return static_cast<double>(read_u16(value.bytes, little_endian));
    case 4U:
        return static_cast<double>(read_u32(value.bytes, little_endian));
    case 5U: {
        if (value.byte_count < 8U) {
            return std::nullopt;
        }
        const std::uint32_t numerator = read_u32(value.bytes, little_endian);
        const std::uint32_t denominator = read_u32(value.bytes + 4U, little_endian);
        if (denominator == 0U) {
            return std::nullopt;
        }
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    default:
        return std::nullopt;
    }
}

template <typename Callback>
void visit_ifd(
    const std::uint8_t* tiff,
    const std::size_t bytes,
    const bool little_endian,
    const std::uint32_t offset,
    Callback&& callback
) {
    if (!in_bounds(offset, 2U, bytes)) {
        return;
    }
    const std::size_t entry_count = read_u16(tiff + offset, little_endian);
    const std::size_t entries_offset = static_cast<std::size_t>(offset) + 2U;
    if (entry_count > (bytes - entries_offset) / 12U) {
        return;
    }
    for (std::size_t index = 0U; index < entry_count; ++index) {
        const std::size_t entry_offset = entries_offset + index * 12U;
        const std::uint16_t tag = read_u16(tiff + entry_offset, little_endian);
        const auto value = tiff_value(tiff, bytes, little_endian, entry_offset);
        if (value.has_value()) {
            callback(tag, *value);
        }
    }
}

} // namespace

void parse_tiff_exif(const std::span<const std::uint8_t> bytes, RasterExif& exif) noexcept {
    if (bytes.size() < 8U) {
        return;
    }
    const auto* const tiff = bytes.data();
    const bool little_endian = tiff[0] == 'I' && tiff[1] == 'I';
    const bool big_endian = tiff[0] == 'M' && tiff[1] == 'M';
    if ((!little_endian && !big_endian) || read_u16(tiff + 2U, little_endian) != 42U) {
        return;
    }
    const std::uint32_t ifd0 = read_u32(tiff + 4U, little_endian);
    std::uint32_t exif_ifd = 0U;
    visit_ifd(tiff, bytes.size(), little_endian, ifd0, [&](const std::uint16_t tag, const TiffValue& value) {
        switch (tag) {
        case 0x010fU:
            copy_ascii(exif.make, sizeof(exif.make), value);
            break;
        case 0x0110U:
            copy_ascii(exif.model, sizeof(exif.model), value);
            break;
        case 0x0112U:
            if (const auto value_number = tiff_scalar(value, little_endian); value_number.has_value()) {
                const auto orientation = static_cast<std::uint16_t>(*value_number);
                if (orientation >= 1U && orientation <= 8U) {
                    exif.orientation = orientation;
                }
            }
            break;
        case 0x8769U:
            if (value.type == 4U && value.byte_count >= 4U) {
                exif_ifd = read_u32(value.bytes, little_endian);
            }
            break;
        default:
            break;
        }
    });
    if (exif_ifd == 0U) {
        return;
    }
    visit_ifd(tiff, bytes.size(), little_endian, exif_ifd, [&](const std::uint16_t tag, const TiffValue& value) {
        const auto number = tiff_scalar(value, little_endian);
        switch (tag) {
        case 0x8827U:
            if (number.has_value()) {
                exif.iso_speed = *number;
            }
            break;
        case 0x829aU:
            if (number.has_value()) {
                exif.exposure_time_seconds = *number;
            }
            break;
        case 0x829dU:
            if (number.has_value()) {
                exif.aperture_f_number = *number;
            }
            break;
        case 0x920aU:
            if (number.has_value()) {
                exif.focal_length_mm = *number;
            }
            break;
        case 0xa405U:
            if (number.has_value()) {
                exif.focal_length_35mm = *number;
            }
            break;
        case 0xa433U:
            copy_ascii(exif.lens_make, sizeof(exif.lens_make), value);
            break;
        case 0xa434U:
            copy_ascii(exif.lens_model, sizeof(exif.lens_model), value);
            break;
        default:
            break;
        }
    });
}

} // namespace shadow::image
