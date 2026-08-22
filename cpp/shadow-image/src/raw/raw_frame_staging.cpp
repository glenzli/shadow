#include <shadow/image/raw_frame_staging.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <span>
#include <sstream>
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
        const bool lower_hex = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        if (hyphen ? value != '-' : !lower_hex) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string hex_encode(const std::string_view value) {
    constexpr std::array<char, 16U> digits{
        '0',
        '1',
        '2',
        '3',
        '4',
        '5',
        '6',
        '7',
        '8',
        '9',
        'a',
        'b',
        'c',
        'd',
        'e',
        'f',
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

[[nodiscard]] std::string hex_decode(const std::string_view value, const char* const label) {
    if (value == "-") {
        return {};
    }
    if (value.empty() || value.size() % 2U != 0U) {
        throw std::runtime_error(std::string("RAW frame staging ") + label + " is not hex");
    }
    const auto nibble = [label](const char character) -> std::uint8_t {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<std::uint8_t>(character - 'a' + 10);
        }
        throw std::runtime_error(std::string("RAW frame staging ") + label + " is not hex");
    };
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t index = 0U; index < value.size(); index += 2U) {
        decoded.push_back(
            static_cast<char>((nibble(value[index]) << 4U) | nibble(value[index + 1U]))
        );
    }
    return decoded;
}

template <typename Value> [[nodiscard]] std::string comma_values(const Value& values) {
    std::ostringstream stream;
    stream << std::setprecision(17);
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            stream << ',';
        }
        stream << values[index];
    }
    return stream.str();
}

template <std::size_t Size>
[[nodiscard]] std::array<double, Size>
parse_double_values(const std::string& text, const char* const label) {
    std::array<double, Size> values{};
    std::istringstream stream(text);
    std::string field;
    for (std::size_t index = 0U; index < Size; ++index) {
        if (!std::getline(stream, field, ',') || field.empty()) {
            throw std::runtime_error(std::string("RAW frame staging ") + label + " is incomplete");
        }
        std::size_t consumed = 0U;
        values[index] = std::stod(field, &consumed);
        if (consumed != field.size() || !std::isfinite(values[index])) {
            throw std::runtime_error(std::string("RAW frame staging ") + label + " is invalid");
        }
    }
    if (std::getline(stream, field, ',')) {
        throw std::runtime_error(std::string("RAW frame staging ") + label + " has extra values");
    }
    return values;
}

template <std::size_t Size>
[[nodiscard]] std::array<std::uint32_t, Size>
parse_u32_values(const std::string& text, const char* const label) {
    std::array<std::uint32_t, Size> values{};
    std::istringstream stream(text);
    std::string field;
    for (std::size_t index = 0U; index < Size; ++index) {
        if (!std::getline(stream, field, ',') || field.empty()) {
            throw std::runtime_error(std::string("RAW frame staging ") + label + " is incomplete");
        }
        std::size_t consumed = 0U;
        const auto value = std::stoull(field, &consumed);
        if (consumed != field.size() || value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(std::string("RAW frame staging ") + label + " is invalid");
        }
        values[index] = static_cast<std::uint32_t>(value);
    }
    if (std::getline(stream, field, ',')) {
        throw std::runtime_error(std::string("RAW frame staging ") + label + " has extra values");
    }
    return values;
}

[[nodiscard]] std::uint32_t parse_u32(const std::string& text, const char* const label) {
    std::size_t consumed = 0U;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("RAW frame staging ") + label + " is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool parse_bool01(const std::string& text, const char* const label) {
    if (text == "0") {
        return false;
    }
    if (text == "1") {
        return true;
    }
    throw std::runtime_error(std::string("RAW frame staging ") + label + " is invalid");
}

[[nodiscard]] std::int32_t parse_i32(const std::string& text, const char* const label) {
    std::size_t consumed = 0U;
    const auto value = std::stoll(text, &consumed);
    if (consumed != text.size() || value < std::numeric_limits<std::int32_t>::min()
        || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string("RAW frame staging ") + label + " is invalid");
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] RawCfaColor parse_color(const char value) {
    switch (value) {
    case 'R':
        return RawCfaColor::red;
    case 'G':
        return RawCfaColor::green;
    case 'B':
        return RawCfaColor::blue;
    default:
        throw std::runtime_error("RAW frame staging CFA is invalid");
    }
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
            const auto source_offset = (top + static_cast<std::size_t>(row)) * storage_width + left;
            const auto source =
                std::span(frame.samples.data() + source_offset, static_cast<std::size_t>(width));
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
        std::array<std::uint32_t, 4U> linear_response{};
        std::array<double, 4U> neutral{};
        for (std::uint32_t row = 0U; row < 2U; ++row) {
            for (std::uint32_t column = 0U; column < 2U; ++column) {
                const auto active_site = static_cast<std::size_t>(row * 2U + column);
                const auto source_site = storage_site(frame.descriptor, row, column);
                cfa[active_site] = color_code(frame.descriptor.bayer_2x2[source_site]);
                black[active_site] = frame.descriptor.black_levels[source_site];
                white[active_site] = frame.descriptor.white_levels[source_site];
                linear_response[active_site] =
                    frame.descriptor.linear_response_limits[source_site];
                neutral[active_site] = frame.descriptor.as_shot_neutral[source_site];
            }
        }

        std::ofstream manifest(manifest_partial, std::ios::binary | std::ios::trunc);
        if (!manifest) {
            throw std::runtime_error("cannot create RAW frame staging manifest");
        }
        manifest << raw_frame_staging_schema
                 << " descriptor_contract=active-camera-colour-response-20260822.1"
                 << " width=" << width << " height=" << height
                 << " cfa=" << std::string(cfa.data(), cfa.size()) << " black=" << black[0] << ','
                 << black[1] << ',' << black[2] << ',' << black[3] << " white=" << white[0] << ','
                 << white[1] << ',' << white[2] << ',' << white[3]
                 << " linear_response=" << linear_response[0] << ',' << linear_response[1] << ','
                 << linear_response[2] << ',' << linear_response[3]
                 << " has_linear_response="
                 << (frame.descriptor.has_linear_response_limits ? 1 : 0)
                 << " orientation=" << frame.descriptor.orientation
                 << " bits_per_sample=" << frame.descriptor.bits_per_sample
                 << " as_shot_neutral=" << comma_values(neutral) << " camera_to_xyz_d50="
                 << (frame.descriptor.has_camera_to_xyz_d50
                         ? comma_values(frame.descriptor.camera_to_xyz_d50)
                         : "-")
                 << " xyz_to_camera_d65="
                 << (frame.descriptor.has_xyz_to_camera_d65
                         ? comma_values(frame.descriptor.xyz_to_camera_d65)
                         : "-")
                 << " camera_to_linear_srgb_d65="
                 << (frame.descriptor.has_camera_to_linear_srgb_d65
                         ? comma_values(frame.descriptor.camera_to_linear_srgb_d65)
                         : "-")
                 << " pending_dng_opcode_bytes="
                 << comma_values(
                        frame.descriptor.declared_pending_corrections.dng_opcode_list_bytes
                    )
                 << " provider_id_hex=" << hex_encode(frame.descriptor.provider_id)
                 << " provider_version_hex=" << hex_encode(frame.descriptor.provider_version)
                 << " sample_bytes=" << sample_bytes << '\n';
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

RawFrame read_raw_frame_staging(const fs::path& manifest_path) {
    if (manifest_path.empty() || !fs::is_regular_file(manifest_path)
        || fs::file_size(manifest_path) == 0U || fs::file_size(manifest_path) > 16U * 1024U) {
        throw std::runtime_error("RAW frame staging manifest is missing or invalid");
    }
    std::ifstream manifest(manifest_path, std::ios::binary);
    const std::string text{
        std::istreambuf_iterator<char>(manifest),
        std::istreambuf_iterator<char>()
    };
    if (manifest.bad() || text.empty() || text.back() != '\n'
        || text.find('\n') != text.size() - 1U) {
        throw std::runtime_error("RAW frame staging manifest is not one complete line");
    }
    std::istringstream tokens(text);
    std::string schema;
    tokens >> schema;
    if (schema != raw_frame_staging_schema) {
        throw std::runtime_error("RAW frame staging schema is unsupported");
    }
    std::map<std::string, std::string> fields;
    std::string token;
    while (tokens >> token) {
        const auto separator = token.find('=');
        if (separator == std::string::npos || separator == 0U || separator + 1U >= token.size()
            || !fields.emplace(token.substr(0U, separator), token.substr(separator + 1U)).second) {
            throw std::runtime_error("RAW frame staging manifest fields are malformed");
        }
    }
    constexpr std::array<std::string_view, 18U> required{
        "width",
        "height",
        "cfa",
        "black",
        "white",
        "linear_response",
        "has_linear_response",
        "orientation",
        "bits_per_sample",
        "as_shot_neutral",
        "camera_to_xyz_d50",
        "xyz_to_camera_d65",
        "camera_to_linear_srgb_d65",
        "pending_dng_opcode_bytes",
        "provider_id_hex",
        "provider_version_hex",
        "sample_bytes",
        "descriptor_contract",
    };
    // The fixed dated marker makes the expanded descriptor fail closed against stale
    // request-private manifests without maintaining a compatibility ladder.
    if (fields.size() != required.size()) {
        throw std::runtime_error("RAW frame staging manifest field set changed");
    }
    for (const auto name : required) {
        if (!fields.contains(std::string(name))) {
            throw std::runtime_error("RAW frame staging manifest is missing a required field");
        }
    }
    if (fields.at("descriptor_contract") != "active-camera-colour-response-20260822.1") {
        throw std::runtime_error("RAW frame staging descriptor contract is unsupported");
    }
    const auto width = parse_u32(fields.at("width"), "width");
    const auto height = parse_u32(fields.at("height"), "height");
    const auto sample_bytes = std::stoull(fields.at("sample_bytes"));
    const auto expected_bytes = static_cast<std::uint64_t>(width) * height * 2U;
    if (width == 0U || height == 0U || sample_bytes != expected_bytes
        || expected_bytes > 512U * 1024U * 1024U) {
        throw std::runtime_error("RAW frame staging sample dimensions are invalid");
    }
    const std::string& cfa = fields.at("cfa");
    if (cfa.size() != 4U) {
        throw std::runtime_error("RAW frame staging CFA is invalid");
    }
    fs::path sample_path = manifest_path;
    sample_path += ".u16le";
    if (!fs::is_regular_file(sample_path) || fs::file_size(sample_path) != expected_bytes) {
        throw std::runtime_error("RAW frame staging sample payload is incomplete");
    }
    std::ifstream samples(sample_path, std::ios::binary);
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(expected_bytes));
    samples.read(
        reinterpret_cast<char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size())
    );
    if (!samples || samples.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("RAW frame staging sample payload could not be read exactly");
    }

    RawFrame frame;
    frame.descriptor.provider_id = hex_decode(fields.at("provider_id_hex"), "provider id");
    frame.descriptor.provider_version =
        hex_decode(fields.at("provider_version_hex"), "provider version");
    frame.descriptor.storage_dimensions = {width, height};
    frame.descriptor.active_dimensions = {width, height};
    frame.descriptor.orientation = parse_i32(fields.at("orientation"), "orientation");
    frame.descriptor.cfa_layout = RawFrameCfaLayout::bayer_2x2;
    frame.descriptor.cfa_pattern = cfa;
    frame.descriptor.bits_per_sample = parse_u32(fields.at("bits_per_sample"), "bits per sample");
    frame.descriptor.black_levels = parse_u32_values<4U>(fields.at("black"), "black levels");
    frame.descriptor.white_levels = parse_u32_values<4U>(fields.at("white"), "white levels");
    frame.descriptor.linear_response_limits =
        parse_u32_values<4U>(fields.at("linear_response"), "linear-response limits");
    frame.descriptor.has_linear_response_limits =
        parse_bool01(fields.at("has_linear_response"), "linear-response availability");
    frame.descriptor.as_shot_neutral =
        parse_double_values<4U>(fields.at("as_shot_neutral"), "as-shot neutral");
    for (std::size_t index = 0U; index < frame.descriptor.bayer_2x2.size(); ++index) {
        frame.descriptor.bayer_2x2[index] = parse_color(cfa[index]);
    }
    if (fields.at("camera_to_xyz_d50") != "-") {
        frame.descriptor.camera_to_xyz_d50 =
            parse_double_values<9U>(fields.at("camera_to_xyz_d50"), "D50 camera matrix");
        frame.descriptor.has_camera_to_xyz_d50 = true;
    }
    if (fields.at("xyz_to_camera_d65") != "-") {
        frame.descriptor.xyz_to_camera_d65 =
            parse_double_values<9U>(fields.at("xyz_to_camera_d65"), "D65 XYZ camera matrix");
        frame.descriptor.has_xyz_to_camera_d65 = true;
    }
    if (fields.at("camera_to_linear_srgb_d65") != "-") {
        frame.descriptor.camera_to_linear_srgb_d65 = parse_double_values<9U>(
            fields.at("camera_to_linear_srgb_d65"),
            "linear sRGB camera matrix"
        );
        frame.descriptor.has_camera_to_linear_srgb_d65 = true;
    }
    frame.descriptor.declared_pending_corrections.dng_opcode_list_bytes =
        parse_u32_values<3U>(fields.at("pending_dng_opcode_bytes"), "DNG opcode sizes");
    frame.samples.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0U; index < frame.samples.size(); ++index) {
        frame.samples[index] = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(encoded[index * 2U])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(encoded[index * 2U + 1U]) << 8U)
        );
    }
    if (!frame.is_bayer_2x2()) {
        throw std::runtime_error("RAW frame staging payload does not form a valid Bayer frame");
    }
    return frame;
}

} // namespace shadow::image
