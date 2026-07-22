#include <shadow/image/lut.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace shadow::image {

namespace {

constexpr std::size_t maximum_document_bytes = 16U * 1'024U * 1'024U;
constexpr std::uint16_t minimum_lut_size = 2U;
constexpr std::uint16_t maximum_lut_size = 65U;

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'
                              || value.front() == '\r')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'
                              || value.back() == '\r')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] double parse_number(const std::string_view token) {
    double result = 0.0;
    const auto [end, error] = std::from_chars(
        token.data(), token.data() + token.size(), result
    );
    if (error != std::errc{} || end != token.data() + token.size()
        || !std::isfinite(result)) {
        throw std::invalid_argument(".cube contains an invalid finite number");
    }
    return result;
}

[[nodiscard]] std::array<double, 3> parse_triplet(const std::string_view value) {
    std::istringstream stream{std::string(value)};
    std::array<std::string, 3> tokens;
    std::string extra;
    if (!(stream >> tokens[0] >> tokens[1] >> tokens[2]) || stream >> extra) {
        throw std::invalid_argument(".cube triplets must contain exactly three numbers");
    }
    return {parse_number(tokens[0]), parse_number(tokens[1]), parse_number(tokens[2])};
}

[[nodiscard]] std::uint16_t parse_size(const std::string_view value) {
    unsigned int parsed = 0U;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed
    );
    if (error != std::errc{} || end != value.data() + value.size()
        || parsed < minimum_lut_size || parsed > maximum_lut_size) {
        throw std::invalid_argument("LUT_3D_SIZE must be an integer from 2 through 65");
    }
    return static_cast<std::uint16_t>(parsed);
}

[[nodiscard]] std::string parse_title(const std::string_view value) {
    const auto trimmed = trim(value);
    if (trimmed.size() < 2U || trimmed.front() != '"' || trimmed.back() != '"') {
        throw std::invalid_argument("TITLE must be enclosed in double quotes");
    }
    const auto title = trimmed.substr(1U, trimmed.size() - 2U);
    if (title.empty() || title.size() > 512U || title.find('"') != std::string_view::npos) {
        throw std::invalid_argument("TITLE must contain 1 through 512 unquoted bytes");
    }
    return std::string(title);
}

[[nodiscard]] std::array<float, 3> entry_at(
    const CubeLut3D& lut,
    const std::size_t red,
    const std::size_t green,
    const std::size_t blue
) {
    const std::size_t size = lut.size;
    return lut.entries[(blue * size + green) * size + red];
}

[[nodiscard]] float lerp(const float left, const float right, const double amount) noexcept {
    return static_cast<float>(
        static_cast<double>(left) * (1.0 - amount) + static_cast<double>(right) * amount
    );
}

} // namespace

CubeLut3D parse_cube_lut(const std::string_view document) {
    if (document.empty() || document.size() > maximum_document_bytes) {
        throw std::invalid_argument(".cube document must contain 1 byte through 16 MiB");
    }

    CubeLut3D result;
    bool saw_size = false;
    bool saw_data = false;
    bool saw_domain_min = false;
    bool saw_domain_max = false;
    std::istringstream lines{std::string(document)};
    std::string owned_line;
    while (std::getline(lines, owned_line)) {
        auto line = trim(owned_line);
        const auto comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = trim(line.substr(0U, comment));
        }
        if (line.empty()) {
            continue;
        }

        const auto separator = line.find_first_of(" \t");
        const auto keyword = line.substr(0U, separator);
        const auto value = separator == std::string_view::npos
            ? std::string_view{}
            : trim(line.substr(separator + 1U));
        if (keyword == "TITLE") {
            if (saw_data || !result.title.empty()) {
                throw std::invalid_argument("TITLE must appear once before LUT data");
            }
            result.title = parse_title(value);
        } else if (keyword == "LUT_3D_SIZE") {
            if (saw_data || saw_size) {
                throw std::invalid_argument("LUT_3D_SIZE must appear once before LUT data");
            }
            result.size = parse_size(value);
            saw_size = true;
        } else if (keyword == "LUT_1D_SIZE") {
            throw std::invalid_argument("1D .cube LUTs are not supported by the 3D LUT node");
        } else if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
            if (saw_data) {
                throw std::invalid_argument("DOMAIN directives must appear before LUT data");
            }
            auto& seen = keyword == "DOMAIN_MIN" ? saw_domain_min : saw_domain_max;
            if (seen) {
                throw std::invalid_argument("DOMAIN directives may appear only once");
            }
            seen = true;
            (keyword == "DOMAIN_MIN" ? result.domain_min : result.domain_max) =
                parse_triplet(value);
        } else if ((keyword.front() >= '0' && keyword.front() <= '9')
                   || keyword.front() == '-' || keyword.front() == '+'
                   || keyword.front() == '.') {
            if (!saw_size) {
                throw std::invalid_argument("LUT_3D_SIZE must precede LUT data");
            }
            saw_data = true;
            const auto triplet = parse_triplet(line);
            result.entries.push_back({
                static_cast<float>(triplet[0]),
                static_cast<float>(triplet[1]),
                static_cast<float>(triplet[2]),
            });
        } else {
            throw std::invalid_argument(".cube contains an unsupported directive");
        }
    }

    if (!saw_size) {
        throw std::invalid_argument(".cube is missing LUT_3D_SIZE");
    }
    const std::size_t size = result.size;
    const std::size_t expected = size * size * size;
    if (result.entries.size() != expected) {
        throw std::invalid_argument(".cube data count does not equal LUT_3D_SIZE cubed");
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        if (!(result.domain_min[channel] < result.domain_max[channel])) {
            throw std::invalid_argument("every .cube DOMAIN_MIN value must be below DOMAIN_MAX");
        }
    }
    return result;
}

std::array<float, 3> sample_cube_lut(
    const CubeLut3D& lut,
    const std::array<float, 3> input
) {
    const std::size_t size = lut.size;
    if (size < minimum_lut_size || size > maximum_lut_size
        || lut.entries.size() != size * size * size) {
        throw std::invalid_argument("cannot sample an invalid 3D LUT");
    }

    std::array<std::size_t, 3> lower{};
    std::array<std::size_t, 3> upper{};
    std::array<double, 3> fraction{};
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        if (!std::isfinite(input[channel])
            || !(lut.domain_min[channel] < lut.domain_max[channel])) {
            throw std::invalid_argument("cannot sample a non-finite input or invalid LUT domain");
        }
        const double normalized = std::clamp(
            (static_cast<double>(input[channel]) - lut.domain_min[channel])
                / (lut.domain_max[channel] - lut.domain_min[channel]),
            0.0,
            1.0
        );
        const double position = normalized * static_cast<double>(size - 1U);
        lower[channel] = static_cast<std::size_t>(std::floor(position));
        upper[channel] = std::min(lower[channel] + 1U, size - 1U);
        fraction[channel] = position - static_cast<double>(lower[channel]);
    }

    const auto c000 = entry_at(lut, lower[0], lower[1], lower[2]);
    const auto c100 = entry_at(lut, upper[0], lower[1], lower[2]);
    const auto c010 = entry_at(lut, lower[0], upper[1], lower[2]);
    const auto c110 = entry_at(lut, upper[0], upper[1], lower[2]);
    const auto c001 = entry_at(lut, lower[0], lower[1], upper[2]);
    const auto c101 = entry_at(lut, upper[0], lower[1], upper[2]);
    const auto c011 = entry_at(lut, lower[0], upper[1], upper[2]);
    const auto c111 = entry_at(lut, upper[0], upper[1], upper[2]);
    std::array<float, 3> result{};
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const float c00 = lerp(c000[channel], c100[channel], fraction[0]);
        const float c10 = lerp(c010[channel], c110[channel], fraction[0]);
        const float c01 = lerp(c001[channel], c101[channel], fraction[0]);
        const float c11 = lerp(c011[channel], c111[channel], fraction[0]);
        const float c0 = lerp(c00, c10, fraction[1]);
        const float c1 = lerp(c01, c11, fraction[1]);
        result[channel] = lerp(c0, c1, fraction[2]);
    }
    return result;
}

} // namespace shadow::image
