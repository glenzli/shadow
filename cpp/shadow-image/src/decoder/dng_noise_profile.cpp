#include "dng_noise_profile.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

inline constexpr std::uint16_t classic_tiff_magic = 42U;
inline constexpr std::uint16_t big_tiff_magic = 43U;
inline constexpr std::uint16_t photometric_cfa = 32'803U;
inline constexpr std::uint16_t tag_new_subfile_type = 254U;
inline constexpr std::uint16_t tag_photometric_interpretation = 262U;
inline constexpr std::uint16_t tag_sub_ifds = 330U;
inline constexpr std::uint16_t tag_dng_version = 50'706U;
inline constexpr std::uint16_t tag_cfa_plane_color = 50'710U;
inline constexpr std::uint16_t tag_linearization_table = 50'712U;
inline constexpr std::uint16_t tag_black_level_delta_h = 50'715U;
inline constexpr std::uint16_t tag_black_level_delta_v = 50'716U;
inline constexpr std::uint16_t tag_noise_profile = 51'041U;

inline constexpr std::uint16_t tiff_type_byte = 1U;
inline constexpr std::uint16_t tiff_type_short = 3U;
inline constexpr std::uint16_t tiff_type_long = 4U;
inline constexpr std::uint16_t tiff_type_double = 12U;
inline constexpr std::uint16_t tiff_type_ifd = 13U;

inline constexpr std::uint32_t maximum_ifd_entries = 1'024U;
inline constexpr std::uint32_t maximum_ifds = 64U;
inline constexpr std::uint32_t maximum_sub_ifds = 32U;
inline constexpr std::uint32_t maximum_color_planes = 8U;

enum class ByteOrder : std::uint8_t {
    little,
    big,
};

struct TiffEntry final {
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
    std::uint64_t entry_offset = 0U;
    std::uint64_t payload_offset = 0U;
    std::uint64_t payload_bytes = 0U;
};

struct IfdSummary final {
    std::uint32_t new_subfile_type = 0U;
    std::optional<std::uint16_t> photometric;
    std::optional<std::array<std::uint8_t, 4U>> dng_version;
    std::vector<std::uint8_t> cfa_plane_colors;
    std::vector<double> noise_profile;
    std::vector<std::uint32_t> sub_ifds;
    std::uint32_t next_ifd = 0U;
    bool has_noise_profile = false;
    bool has_unsupported_linear_mapping = false;
};

[[nodiscard]] bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result
) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool
checked_add(const std::uint64_t left, const std::uint64_t right, std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::uint64_t tiff_type_bytes(const std::uint16_t type) noexcept {
    switch (type) {
    case 1U:
    case 2U:
    case 6U:
    case 7U:
        return 1U;
    case 3U:
    case 8U:
        return 2U;
    case 4U:
    case 9U:
    case 11U:
    case 13U:
        return 4U;
    case 5U:
    case 10U:
    case 12U:
        return 8U;
    default:
        return 0U;
    }
}

class MemorySource final {
  public:
    explicit MemorySource(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] bool
    read(const std::uint64_t offset, const std::span<std::uint8_t> output) noexcept {
        if (offset > bytes_.size() || output.size() > bytes_.size() - offset) {
            return false;
        }
        std::copy_n(bytes_.data() + static_cast<std::size_t>(offset), output.size(), output.data());
        return true;
    }

  private:
    std::span<const std::uint8_t> bytes_;
};

class FileSource final {
  public:
    explicit FileSource(const std::filesystem::path& path) {
        std::error_code error;
        const auto bytes = std::filesystem::file_size(path, error);
        if (error) {
            return;
        }
        size_ = bytes;
        stream_.open(path, std::ios::binary);
    }

    [[nodiscard]] bool available() const noexcept {
        return stream_.is_open();
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool
    read(const std::uint64_t offset, const std::span<std::uint8_t> output) noexcept {
        if (!available() || offset > size_ || output.size() > size_ - offset
            || offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())
            || output.size()
                   > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return false;
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) {
            return false;
        }
        stream_.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size())
        );
        return stream_.good()
               || (stream_.eof()
                   && stream_.gcount() == static_cast<std::streamsize>(output.size()));
    }

  private:
    std::ifstream stream_;
    std::uint64_t size_ = 0U;
};

template <typename Source>
[[nodiscard]] bool read_bytes(
    Source& source,
    const std::uint64_t offset,
    const std::span<std::uint8_t> output
) noexcept {
    return offset <= source.size() && output.size() <= source.size() - offset
           && source.read(offset, output);
}

template <typename Source>
[[nodiscard]] std::optional<std::uint16_t>
read_u16(Source& source, const std::uint64_t offset, const ByteOrder order) noexcept {
    std::array<std::uint8_t, 2U> bytes{};
    if (!read_bytes(source, offset, bytes)) {
        return std::nullopt;
    }
    if (order == ByteOrder::little) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U)
        );
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) | static_cast<std::uint16_t>(bytes[1])
    );
}

template <typename Source>
[[nodiscard]] std::optional<std::uint32_t>
read_u32(Source& source, const std::uint64_t offset, const ByteOrder order) noexcept {
    std::array<std::uint8_t, 4U> bytes{};
    if (!read_bytes(source, offset, bytes)) {
        return std::nullopt;
    }
    std::uint32_t result = 0U;
    if (order == ByteOrder::little) {
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            result |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
        }
    } else {
        for (const auto byte : bytes) {
            result = (result << 8U) | byte;
        }
    }
    return result;
}

template <typename Source>
[[nodiscard]] std::optional<std::uint64_t>
read_u64(Source& source, const std::uint64_t offset, const ByteOrder order) noexcept {
    std::array<std::uint8_t, 8U> bytes{};
    if (!read_bytes(source, offset, bytes)) {
        return std::nullopt;
    }
    std::uint64_t result = 0U;
    if (order == ByteOrder::little) {
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
        }
    } else {
        for (const auto byte : bytes) {
            result = (result << 8U) | byte;
        }
    }
    return result;
}

template <typename Source>
[[nodiscard]] std::optional<TiffEntry>
read_entry(Source& source, const std::uint64_t entry_offset, const ByteOrder order) noexcept {
    const auto tag = read_u16(source, entry_offset, order);
    const auto type = read_u16(source, entry_offset + 2U, order);
    const auto count = read_u32(source, entry_offset + 4U, order);
    if (!tag.has_value() || !type.has_value() || !count.has_value() || *count == 0U) {
        return std::nullopt;
    }
    const std::uint64_t unit = tiff_type_bytes(*type);
    std::uint64_t payload_bytes = 0U;
    if (unit == 0U || !checked_multiply(*count, unit, payload_bytes)) {
        return std::nullopt;
    }
    std::uint64_t payload_offset = entry_offset + 8U;
    if (payload_bytes > 4U) {
        const auto stored_offset = read_u32(source, entry_offset + 8U, order);
        if (!stored_offset.has_value()) {
            return std::nullopt;
        }
        payload_offset = *stored_offset;
    }
    if (payload_offset > source.size() || payload_bytes > source.size() - payload_offset) {
        return std::nullopt;
    }
    return TiffEntry{
        .tag = *tag,
        .type = *type,
        .count = *count,
        .entry_offset = entry_offset,
        .payload_offset = payload_offset,
        .payload_bytes = payload_bytes,
    };
}

template <typename Source>
[[nodiscard]] bool scalar_u16(
    Source& source,
    const TiffEntry& entry,
    const ByteOrder order,
    std::uint16_t& output
) noexcept {
    if (entry.type != tiff_type_short || entry.count != 1U) {
        return false;
    }
    const auto value = read_u16(source, entry.payload_offset, order);
    if (!value.has_value()) {
        return false;
    }
    output = *value;
    return true;
}

template <typename Source>
[[nodiscard]] bool scalar_u32(
    Source& source,
    const TiffEntry& entry,
    const ByteOrder order,
    std::uint32_t& output
) noexcept {
    if (entry.type != tiff_type_long || entry.count != 1U) {
        return false;
    }
    const auto value = read_u32(source, entry.payload_offset, order);
    if (!value.has_value()) {
        return false;
    }
    output = *value;
    return true;
}

template <typename Source>
[[nodiscard]] bool read_byte_values(
    Source& source,
    const TiffEntry& entry,
    const std::uint32_t maximum_count,
    std::vector<std::uint8_t>& output
) {
    if (entry.type != tiff_type_byte || entry.count > maximum_count) {
        return false;
    }
    output.resize(entry.count);
    return read_bytes(source, entry.payload_offset, output);
}

template <typename Source>
[[nodiscard]] bool read_long_values(
    Source& source,
    const TiffEntry& entry,
    const ByteOrder order,
    const std::uint32_t maximum_count,
    std::vector<std::uint32_t>& output
) {
    if ((entry.type != tiff_type_long && entry.type != tiff_type_ifd)
        || entry.count > maximum_count) {
        return false;
    }
    output.clear();
    output.reserve(entry.count);
    for (std::uint32_t index = 0U; index < entry.count; ++index) {
        const auto value =
            read_u32(source, entry.payload_offset + static_cast<std::uint64_t>(index) * 4U, order);
        if (!value.has_value()) {
            return false;
        }
        output.push_back(*value);
    }
    return true;
}

template <typename Source>
[[nodiscard]] bool read_double_values(
    Source& source,
    const TiffEntry& entry,
    const ByteOrder order,
    std::vector<double>& output
) {
    const std::uint32_t maximum_count = maximum_color_planes * 2U;
    if (entry.type != tiff_type_double || entry.count > maximum_count) {
        return false;
    }
    output.clear();
    output.reserve(entry.count);
    for (std::uint32_t index = 0U; index < entry.count; ++index) {
        const auto bits =
            read_u64(source, entry.payload_offset + static_cast<std::uint64_t>(index) * 8U, order);
        if (!bits.has_value()) {
            return false;
        }
        output.push_back(std::bit_cast<double>(*bits));
    }
    return true;
}

template <typename Source>
[[nodiscard]] bool parse_ifd(
    Source& source,
    const std::uint32_t ifd_offset,
    const ByteOrder order,
    IfdSummary& summary
) {
    const auto count = read_u16(source, ifd_offset, order);
    if (!count.has_value() || *count > maximum_ifd_entries) {
        return false;
    }
    std::uint64_t entries_bytes = 0U;
    std::uint64_t entries_offset = 0U;
    std::uint64_t next_offset_position = 0U;
    if (!checked_multiply(*count, 12U, entries_bytes)
        || !checked_add(ifd_offset, 2U, entries_offset)
        || !checked_add(entries_offset, entries_bytes, next_offset_position)
        || next_offset_position > source.size() || 4U > source.size() - next_offset_position) {
        return false;
    }
    const std::uint64_t structure_end = next_offset_position + 4U;

    std::optional<std::uint16_t> previous_tag;
    for (std::uint32_t index = 0U; index < *count; ++index) {
        const std::uint64_t entry_offset = entries_offset + static_cast<std::uint64_t>(index) * 12U;
        const auto entry = read_entry(source, entry_offset, order);
        if (!entry.has_value() || (previous_tag.has_value() && entry->tag <= *previous_tag)) {
            return false;
        }
        if (entry->payload_bytes > 4U
            && (entry->payload_offset < 8U
                || (entry->payload_offset < structure_end
                    && ifd_offset < entry->payload_offset + entry->payload_bytes))) {
            return false;
        }
        previous_tag = entry->tag;
        switch (entry->tag) {
        case tag_new_subfile_type:
            if (!scalar_u32(source, *entry, order, summary.new_subfile_type)) {
                return false;
            }
            break;
        case tag_photometric_interpretation: {
            std::uint16_t value = 0U;
            if (!scalar_u16(source, *entry, order, value)) {
                return false;
            }
            summary.photometric = value;
            break;
        }
        case tag_sub_ifds:
            if (!read_long_values(source, *entry, order, maximum_sub_ifds, summary.sub_ifds)) {
                return false;
            }
            break;
        case tag_dng_version: {
            std::vector<std::uint8_t> version;
            if (entry->count != 4U || !read_byte_values(source, *entry, 4U, version)) {
                return false;
            }
            summary.dng_version = std::array{
                version[0],
                version[1],
                version[2],
                version[3],
            };
            break;
        }
        case tag_cfa_plane_color:
            if (!read_byte_values(source, *entry, maximum_color_planes, summary.cfa_plane_colors)) {
                return false;
            }
            break;
        case tag_noise_profile:
            summary.has_noise_profile = true;
            if (!read_double_values(source, *entry, order, summary.noise_profile)) {
                return false;
            }
            break;
        case tag_linearization_table:
        case tag_black_level_delta_h:
        case tag_black_level_delta_v:
            summary.has_unsupported_linear_mapping = true;
            break;
        default:
            break;
        }
    }
    const auto next = read_u32(source, next_offset_position, order);
    if (!next.has_value()) {
        return false;
    }
    summary.next_ifd = *next;
    return true;
}

[[nodiscard]] bool valid_dng_version(const std::array<std::uint8_t, 4U>& version) noexcept {
    return version[0] == 1U && version[1] <= 7U;
}

[[nodiscard]] std::optional<DngNoiseProfile>
canonical_profile(const IfdSummary& raw_ifd, bool& unsupported_shape) {
    unsupported_shape = false;
    if (!raw_ifd.has_noise_profile) {
        return std::nullopt;
    }
    const auto& values = raw_ifd.noise_profile;
    DngNoiseProfile profile;
    if (values.size() == 2U) {
        profile.normalized_scale.fill(values[0]);
        profile.normalized_offset.fill(values[1]);
        return profile.valid() ? std::optional(profile) : std::nullopt;
    }
    if (values.size() != 6U
        || (!raw_ifd.cfa_plane_colors.empty() && raw_ifd.cfa_plane_colors.size() != 3U)) {
        unsupported_shape = true;
        return std::nullopt;
    }
    constexpr std::array<std::uint8_t, 3U> default_rgb_planes{0U, 1U, 2U};
    const std::span<const std::uint8_t> plane_colors =
        raw_ifd.cfa_plane_colors.empty() ? std::span<const std::uint8_t>(default_rgb_planes)
                                         : std::span<const std::uint8_t>(raw_ifd.cfa_plane_colors);
    std::array<bool, 3U> assigned{};
    for (std::size_t plane = 0U; plane < 3U; ++plane) {
        const std::uint8_t color = plane_colors[plane];
        if (color > 2U || assigned[color]) {
            unsupported_shape = true;
            return std::nullopt;
        }
        assigned[color] = true;
        profile.normalized_scale[color] = values[plane * 2U];
        profile.normalized_offset[color] = values[plane * 2U + 1U];
    }
    if (!std::ranges::all_of(assigned, [](const bool value) { return value; })) {
        unsupported_shape = true;
        return std::nullopt;
    }
    return profile.valid() ? std::optional(profile) : std::nullopt;
}

template <typename Source> [[nodiscard]] DngNoiseProfileReceipt parse_source(Source& source) {
    DngNoiseProfileReceipt receipt;
    std::array<std::uint8_t, 4U> header{};
    if (!read_bytes(source, 0U, header)) {
        receipt.status = DngNoiseProfileStatus::not_dng;
        return receipt;
    }
    ByteOrder order;
    if (header[0] == 'I' && header[1] == 'I') {
        order = ByteOrder::little;
    } else if (header[0] == 'M' && header[1] == 'M') {
        order = ByteOrder::big;
    } else {
        receipt.status = DngNoiseProfileStatus::not_dng;
        return receipt;
    }
    const auto magic = read_u16(source, 2U, order);
    if (!magic.has_value()) {
        receipt.status = DngNoiseProfileStatus::not_dng;
        return receipt;
    }
    if (*magic == big_tiff_magic) {
        receipt.status = DngNoiseProfileStatus::unsupported_tiff_layout;
        return receipt;
    }
    if (*magic != classic_tiff_magic) {
        receipt.status = DngNoiseProfileStatus::not_dng;
        return receipt;
    }
    const auto first_ifd = read_u32(source, 4U, order);
    if (!first_ifd.has_value() || *first_ifd < 8U) {
        receipt.status = DngNoiseProfileStatus::malformed;
        return receipt;
    }

    IfdSummary root;
    if (!parse_ifd(source, *first_ifd, order, root)) {
        receipt.status = DngNoiseProfileStatus::malformed;
        return receipt;
    }
    receipt.ifds_visited = 1U;
    if (!root.dng_version.has_value()) {
        receipt.status = DngNoiseProfileStatus::not_dng;
        return receipt;
    }
    if (!valid_dng_version(*root.dng_version)) {
        receipt.status = DngNoiseProfileStatus::unsupported_tiff_layout;
        return receipt;
    }

    std::vector<std::pair<std::uint32_t, IfdSummary>> ifds;
    ifds.emplace_back(*first_ifd, std::move(root));
    std::vector<std::uint32_t> pending;
    if (ifds.front().second.next_ifd != 0U) {
        pending.push_back(ifds.front().second.next_ifd);
    }
    for (const auto offset : ifds.front().second.sub_ifds) {
        if (offset != 0U) {
            pending.push_back(offset);
        }
    }

    while (!pending.empty()) {
        if (ifds.size() >= maximum_ifds) {
            receipt.status = DngNoiseProfileStatus::malformed;
            return receipt;
        }
        const std::uint32_t offset = pending.back();
        pending.pop_back();
        if (std::ranges::any_of(ifds, [offset](const auto& parsed) {
                return parsed.first == offset;
            })) {
            receipt.status = DngNoiseProfileStatus::malformed;
            return receipt;
        }
        IfdSummary summary;
        if (!parse_ifd(source, offset, order, summary)) {
            receipt.status = DngNoiseProfileStatus::malformed;
            return receipt;
        }
        if (summary.next_ifd != 0U) {
            pending.push_back(summary.next_ifd);
        }
        for (const auto child : summary.sub_ifds) {
            if (child != 0U) {
                pending.push_back(child);
            }
        }
        ifds.emplace_back(offset, std::move(summary));
        receipt.ifds_visited = static_cast<std::uint32_t>(ifds.size());
    }

    std::vector<const IfdSummary*> raw_ifds;
    for (const auto& [offset, ifd] : ifds) {
        static_cast<void>(offset);
        if (ifd.new_subfile_type == 0U && ifd.photometric == photometric_cfa) {
            raw_ifds.push_back(&ifd);
        }
    }
    if (raw_ifds.size() > 1U) {
        receipt.status = DngNoiseProfileStatus::ambiguous_raw_ifd;
        return receipt;
    }
    if (raw_ifds.empty() || !raw_ifds.front()->has_noise_profile) {
        receipt.status = DngNoiseProfileStatus::absent;
        return receipt;
    }
    if ((*ifds.front().second.dng_version)[1] < 3U) {
        receipt.status = DngNoiseProfileStatus::malformed;
        return receipt;
    }
    if (raw_ifds.front()->has_unsupported_linear_mapping) {
        receipt.status = DngNoiseProfileStatus::unsupported_profile_shape;
        return receipt;
    }
    bool unsupported_shape = false;
    const auto profile = canonical_profile(*raw_ifds.front(), unsupported_shape);
    if (!profile.has_value()) {
        receipt.status = unsupported_shape ? DngNoiseProfileStatus::unsupported_profile_shape
                                           : DngNoiseProfileStatus::malformed;
        return receipt;
    }
    receipt.status = DngNoiseProfileStatus::exact;
    receipt.profile = *profile;
    return receipt;
}

[[nodiscard]] std::optional<std::size_t> rgb_index(const RawCfaColor color) noexcept {
    switch (color) {
    case RawCfaColor::red:
        return 0U;
    case RawCfaColor::green:
        return 1U;
    case RawCfaColor::blue:
        return 2U;
    case RawCfaColor::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

bool DngNoiseProfile::valid() const noexcept {
    for (std::size_t index = 0U; index < normalized_scale.size(); ++index) {
        if (!std::isfinite(normalized_scale[index]) || !std::isfinite(normalized_offset[index])
            || normalized_scale[index] <= 0.0 || normalized_offset[index] < 0.0) {
            return false;
        }
    }
    return true;
}

bool DngNoiseProfileReceipt::exact() const noexcept {
    return status == DngNoiseProfileStatus::exact && profile.valid();
}

DngNoiseProfileReceipt parse_dng_noise_profile(const std::span<const std::uint8_t> bytes) noexcept {
    try {
        MemorySource source(bytes);
        return parse_source(source);
    } catch (...) {
        return DngNoiseProfileReceipt{.status = DngNoiseProfileStatus::malformed};
    }
}

DngNoiseProfileReceipt read_dng_noise_profile(const std::filesystem::path& path) noexcept {
    try {
        FileSource source(path);
        if (!source.available()) {
            return DngNoiseProfileReceipt{.status = DngNoiseProfileStatus::io_error};
        }
        return parse_source(source);
    } catch (...) {
        return DngNoiseProfileReceipt{.status = DngNoiseProfileStatus::io_error};
    }
}

RawSensorNoiseCalibration resolve_dng_noise_profile(
    const DngNoiseProfileReceipt& receipt,
    const RawFrameDescriptor& descriptor,
    const double iso_sensitivity
) noexcept {
    RawSensorNoiseCalibration calibration;
    if (!receipt.exact() || !descriptor.sensor_noise.valid()
        || descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2 || !std::isfinite(iso_sensitivity)
        || iso_sensitivity <= 0.0) {
        return calibration;
    }
    calibration.model = RawSensorNoiseModel::poisson_gaussian_per_cfa;
    calibration.source = RawSensorNoiseCalibrationSource::embedded_metadata;
    calibration.iso_sensitivity = iso_sensitivity;
    for (std::size_t site = 0U; site < descriptor.bayer_2x2.size(); ++site) {
        const auto color = rgb_index(descriptor.bayer_2x2[site]);
        if (!color.has_value() || descriptor.white_levels[site] <= descriptor.black_levels[site]) {
            return RawSensorNoiseCalibration{};
        }
        const double range =
            static_cast<double>(descriptor.white_levels[site] - descriptor.black_levels[site]);
        calibration.read_noise_stddev_dn[site] =
            std::sqrt(receipt.profile.normalized_offset[*color]) * range;
        calibration.shot_noise_variance_per_dn[site] =
            receipt.profile.normalized_scale[*color] * range;
    }
    return calibration.valid() ? calibration : RawSensorNoiseCalibration{};
}

} // namespace shadow::image
