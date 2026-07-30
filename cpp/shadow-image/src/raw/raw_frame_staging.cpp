#include <shadow/image/raw_frame_staging.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace shadow::image {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool valid_nonce(const std::string_view nonce) noexcept {
    if (nonce.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0U; index < nonce.size(); ++index) {
        const bool hyphen = index == 8U || index == 13U || index == 18U || index == 23U;
        const char value = nonce[index];
        const bool lower_hex =
            (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        if (hyphen ? value != '-' : !lower_hex) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string hex_encode(const std::string_view value) {
    constexpr std::array<char, 16U> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const char item : value) {
        const auto byte = static_cast<unsigned char>(item);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded.empty() ? "-" : encoded;
}

[[nodiscard]] char color_code(const RawCfaColor color) {
    switch (color) {
    case RawCfaColor::red:
        return 'R';
    case RawCfaColor::green:
        return 'G';
    case RawCfaColor::blue:
        return 'B';
    case RawCfaColor::unknown:
        break;
    }
    throw std::runtime_error("RAW frame staging requires a known Bayer colour");
}

[[nodiscard]] std::size_t storage_site(
    const RawFrameDescriptor& descriptor,
    const std::uint32_t active_row,
    const std::uint32_t active_column
) {
    const auto row = (descriptor.active_margins.top + active_row) % 2U;
    const auto column = (descriptor.active_margins.left + active_column) % 2U;
    return static_cast<std::size_t>(row * 2U + column);
}

void remove_if_present(const fs::path& path) noexcept {
    std::error_code ignored;
    fs::remove(path, ignored);
}

void rename_new_file(const fs::path& temporary, const fs::path& destination) {
    if (fs::exists(destination)) {
        throw std::runtime_error("RAW frame staging destination already exists");
    }
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) {
        throw std::runtime_error("cannot publish RAW frame staging file: " + error.message());
    }
}

} // namespace

RawFrameStagingReceipt write_raw_frame_staging(
    const RawFrame& frame,
    const fs::path& manifest_path,
    const std::string_view nonce
) {
    if (!valid_nonce(nonce)) {
        throw std::runtime_error("RAW frame staging nonce must be a lowercase UUID");
    }
    if (!frame.is_bayer_2x2()) {
        throw std::runtime_error("RAW frame staging requires a valid 2x2 Bayer frame");
    }
    if (manifest_path.empty() || manifest_path.filename().empty()) {
        throw std::runtime_error("RAW frame staging manifest path is empty");
    }

    const auto width = frame.descriptor.active_dimensions.width;
    const auto height = frame.descriptor.active_dimensions.height;
    const auto sample_count = static_cast<std::uint64_t>(width) * height;
    if (sample_count > std::numeric_limits<std::uint64_t>::max() / 2U) {
        throw std::runtime_error("RAW frame staging sample count overflow");
    }
    const auto sample_bytes = sample_count * 2U;
    fs::path sample_path = manifest_path;
    sample_path += ".u16le";
    fs::path manifest_partial = manifest_path;
    manifest_partial += ".partial-";
    manifest_partial += nonce;
    fs::path sample_partial = sample_path;
    sample_partial += ".partial-";
    sample_partial += nonce;

    std::error_code error;
    const auto parent = manifest_path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            throw std::runtime_error(
                "cannot create RAW frame staging directory: " + error.message()
            );
        }
    }
    if (fs::exists(manifest_path) || fs::exists(sample_path)) {
        throw std::runtime_error("RAW frame staging destination already exists");
    }

    try {
        std::ofstream samples(sample_partial, std::ios::binary | std::ios::trunc);
        if (!samples) {
            throw std::runtime_error("cannot create RAW frame staging samples");
        }
        std::vector<std::uint8_t> encoded_row(static_cast<std::size_t>(width) * 2U);
        const auto storage_width =
            static_cast<std::size_t>(frame.descriptor.storage_dimensions.width);
        const auto left = static_cast<std::size_t>(frame.descriptor.active_margins.left);
        const auto top = static_cast<std::size_t>(frame.descriptor.active_margins.top);
        for (std::uint32_t row = 0U; row < height; ++row) {
            const auto source_offset =
                (top + static_cast<std::size_t>(row)) * storage_width + left;
            const auto source = std::span(
                frame.samples.data() + source_offset,
                static_cast<std::size_t>(width)
            );
            for (std::size_t column = 0U; column < source.size(); ++column) {
                const auto value = source[column];
                encoded_row[column * 2U] = static_cast<std::uint8_t>(value & 0xffU);
                encoded_row[column * 2U + 1U] = static_cast<std::uint8_t>(value >> 8U);
            }
            samples.write(
                reinterpret_cast<const char*>(encoded_row.data()),
                static_cast<std::streamsize>(encoded_row.size())
            );
        }
        samples.close();
        if (!samples) {
            throw std::runtime_error("cannot write RAW frame staging samples");
        }

        std::array<char, 4U> cfa{};
        std::array<std::uint32_t, 4U> black{};
        std::array<std::uint32_t, 4U> white{};
        for (std::uint32_t row = 0U; row < 2U; ++row) {
            for (std::uint32_t column = 0U; column < 2U; ++column) {
                const auto active_site = static_cast<std::size_t>(row * 2U + column);
                const auto source_site = storage_site(frame.descriptor, row, column);
                cfa[active_site] = color_code(frame.descriptor.bayer_2x2[source_site]);
                black[active_site] = frame.descriptor.black_levels[source_site];
                white[active_site] = frame.descriptor.white_levels[source_site];
            }
        }

        std::ofstream manifest(manifest_partial, std::ios::binary | std::ios::trunc);
        if (!manifest) {
            throw std::runtime_error("cannot create RAW frame staging manifest");
        }
        manifest
            << raw_frame_staging_schema
            << " width=" << width
            << " height=" << height
            << " cfa=" << std::string(cfa.data(), cfa.size())
            << " black=" << black[0] << ',' << black[1] << ',' << black[2] << ',' << black[3]
            << " white=" << white[0] << ',' << white[1] << ',' << white[2] << ',' << white[3]
            << " provider_id_hex=" << hex_encode(frame.descriptor.provider_id)
            << " provider_version_hex=" << hex_encode(frame.descriptor.provider_version)
            << " sample_bytes=" << sample_bytes
            << '\n';
        manifest.close();
        if (!manifest) {
            throw std::runtime_error("cannot write RAW frame staging manifest");
        }

        rename_new_file(sample_partial, sample_path);
        try {
            rename_new_file(manifest_partial, manifest_path);
        } catch (...) {
            remove_if_present(sample_path);
            throw;
        }
    } catch (...) {
        remove_if_present(sample_partial);
        remove_if_present(manifest_partial);
        throw;
    }

    return {
        .manifest_path = manifest_path,
        .sample_path = sample_path,
        .width = width,
        .height = height,
        .sample_bytes = sample_bytes,
    };
}

} // namespace shadow::image
