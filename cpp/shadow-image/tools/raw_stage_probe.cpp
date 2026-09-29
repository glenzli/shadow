#include <shadow/image/decoder.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include "../src/raw/bayer_sampling.hpp"
#include "../src/raw/raw_frame_development_plan.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace image = shadow::image;

namespace {

struct ProbeLocation final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    bool from_sdk_coordinates = false;
};

[[nodiscard]] std::uint32_t parse_coordinate(const char* const value, const char* const label) {
    std::uint32_t parsed = 0U;
    const auto* const begin = value;
    const auto* const end = begin + std::char_traits<char>::length(begin);
    const auto [pointer, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || pointer != end) {
        throw std::runtime_error(std::string("invalid ") + label + " coordinate: " + value);
    }
    return parsed;
}

[[nodiscard]] std::size_t cfa_site(const std::uint32_t x, const std::uint32_t y) noexcept {
    return static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
}

[[nodiscard]] std::string_view cfa_name(const image::RawCfaColor color) noexcept {
    switch (color) {
    case image::RawCfaColor::red:
        return "R";
    case image::RawCfaColor::green:
        return "G";
    case image::RawCfaColor::blue:
        return "B";
    case image::RawCfaColor::unknown:
        return "?";
    }
    return "?";
}

template <typename Number, std::size_t Count>
void print_vector(const std::string_view label, const std::array<Number, Count>& values) {
    std::cout << label << "=(";
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        std::cout << values[index];
    }
    std::cout << ")\n";
}

void print_matrix(const std::string_view label, const std::array<double, 9U>& matrix) {
    std::cout << label << "=(";
    for (std::size_t row = 0U; row < 3U; ++row) {
        if (row != 0U) {
            std::cout << ';';
        }
        for (std::size_t column = 0U; column < 3U; ++column) {
            if (column != 0U) {
                std::cout << ',';
            }
            std::cout << matrix[row * 3U + column];
        }
    }
    std::cout << ")\n";
}

[[nodiscard]] std::array<float, 3U> apply_matrix(
    const std::array<float, 3U>& camera_rgb,
    const std::array<double, 9U>& matrix
) noexcept {
    std::array<float, 3U> result{};
    for (std::size_t output = 0U; output < result.size(); ++output) {
        for (std::size_t input = 0U; input < camera_rgb.size(); ++input) {
            result[output] += static_cast<float>(matrix[output * 3U + input] * camera_rgb[input]);
        }
    }
    return result;
}

[[nodiscard]] ProbeLocation brightest_location(const image::RawFrame& frame) {
    const auto width = frame.descriptor.storage_dimensions.width;
    const auto& active = frame.descriptor.active_dimensions;
    const auto& margins = frame.descriptor.active_margins;
    if (active.width < 7U || active.height < 7U) {
        throw std::runtime_error("RAW frame is too small for a local-stage trace");
    }
    std::uint16_t brightest = 0U;
    ProbeLocation location{.x = margins.left + 3U, .y = margins.top + 3U};
    const auto active_right = margins.left + active.width;
    const auto active_bottom = margins.top + active.height;
    for (std::uint32_t y = margins.top + 3U; y + 3U < active_bottom; ++y) {
        for (std::uint32_t x = margins.left + 3U; x + 3U < active_right; ++x) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            if (frame.samples[index] > brightest) {
                brightest = frame.samples[index];
                location = {.x = x, .y = y};
            }
        }
    }
    return location;
}

void validate_location(const image::RawFrame& frame, const ProbeLocation location) {
    const auto width = frame.descriptor.storage_dimensions.width;
    const auto height = frame.descriptor.storage_dimensions.height;
    if (location.x < 3U || location.y < 3U || location.x + 3U >= width || location.y + 3U >= height) {
        throw std::runtime_error("probe point needs a three-pixel sensor border");
    }
}

void print_sensor_window(const image::RawFrame& frame, const ProbeLocation location) {
    const auto& descriptor = frame.descriptor;
    const auto width = descriptor.storage_dimensions.width;
    std::cout << "sensor_window.center=" << location.x << ',' << location.y << "\n";
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        std::cout << "sensor_window.row" << dy << '=';
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const auto x = static_cast<std::uint32_t>(static_cast<std::int64_t>(location.x) + dx);
            const auto y = static_cast<std::uint32_t>(static_cast<std::int64_t>(location.y) + dy);
            const auto site = cfa_site(x, y);
            const auto sample = frame.samples[static_cast<std::size_t>(y) * width + x];
            const double normalized = (static_cast<double>(sample) - descriptor.black_levels[site])
                                      / (descriptor.white_levels[site] - descriptor.black_levels[site]);
            if (dx != -1) {
                std::cout << ' ';
            }
            std::cout << cfa_name(descriptor.bayer_2x2[site]) << ':' << sample << ':' << normalized;
        }
        std::cout << '\n';
    }
}

void trace(const fs::path& input_path, std::optional<ProbeLocation> requested_location) {
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input_path);
    if (!session->capabilities().raw_frame) {
        throw std::runtime_error("selected provider does not expose RawFrame");
    }
    const image::RawFrame frame = session->decode_raw_frame();
    if (!frame.is_bayer_2x2()) {
        throw std::runtime_error("stage trace requires a valid Bayer RawFrame");
    }

    ProbeLocation location = requested_location.value_or(brightest_location(frame));
    if (location.from_sdk_coordinates) {
        // Confirmed only as a diagnostic convention for the current Nikon Z9 probe family:
        // SDK(x,y) = CFA(y+8, x+12).  This conversion is deliberately local to the tool; it is
        // not a production image-orientation policy.
        const auto sdk_x = location.x;
        const auto sdk_y = location.y;
        location.x = sdk_y + 12U;
        location.y = sdk_x + 8U;
        location.from_sdk_coordinates = false;
        std::cout << "sdk_mapping.assumed=SDK(x,y)=CFA(y+8,x+12)\n"
                  << "sdk_mapping.sdk_point=" << sdk_x << ',' << sdk_y << '\n';
    }
    validate_location(frame, location);

    const image::RawWhiteBalance as_shot{};
    const image::RawFrameLinearTransform transform =
        image::raw_pipeline_detail::prepare_raw_frame_linear_transform(
            frame.descriptor,
            as_shot,
            nullptr
        );

    std::cout << std::fixed << std::setprecision(7)
              << "input=" << input_path.string() << '\n'
              << "provider=" << provider->info().id << '@' << provider->info().version << '\n'
              << "storage_dimensions=" << frame.descriptor.storage_dimensions.width << 'x'
              << frame.descriptor.storage_dimensions.height << '\n'
              << "active_dimensions=" << frame.descriptor.active_dimensions.width << 'x'
              << frame.descriptor.active_dimensions.height << '\n'
              << "active_margins=" << frame.descriptor.active_margins.left << ','
              << frame.descriptor.active_margins.top << ',' << frame.descriptor.active_margins.right
              << ',' << frame.descriptor.active_margins.bottom << '\n'
              << "cfa_pattern=" << frame.descriptor.cfa_pattern << '\n'
              << "stage_note=RawFrame -> normalized CFA -> CFA white balance -> demosaic -> linear sRGB matrix\n";
    print_vector("raw.black_levels", frame.descriptor.black_levels);
    print_vector("raw.white_levels", frame.descriptor.white_levels);
    print_vector("raw.as_shot_neutral", frame.descriptor.as_shot_neutral);
    print_vector("transform.camera_neutral", transform.camera_neutral);
    print_vector("transform.cfa_white_balance", transform.cfa_white_balance);
    print_matrix("transform.camera_to_linear_srgb_d65", transform.camera_to_linear_srgb_d65);
    print_sensor_window(frame, location);

    const auto bilinear_native = image::detail::bilinear_camera_rgb_sample_at(frame, location.x, location.y);
    const auto bilinear_balanced =
        image::detail::bilinear_camera_rgb_sample_at(frame, location.x, location.y, &transform);
    const auto edge_native = image::detail::edge_aware_camera_rgb_sample_at(frame, location.x, location.y);
    const auto edge_balanced =
        image::detail::edge_aware_camera_rgb_sample_at(frame, location.x, location.y, &transform);
    print_vector("stage.camera_rgb.bilinear.native", bilinear_native.values);
    print_vector("stage.camera_rgb.bilinear.cfa_white_balanced", bilinear_balanced.values);
    print_vector("stage.camera_rgb.edge_aware.native", edge_native.values);
    print_vector("stage.camera_rgb.edge_aware.cfa_white_balanced", edge_balanced.values);
    print_vector(
        "stage.linear_srgb.bilinear",
        apply_matrix(bilinear_balanced.values, transform.camera_to_linear_srgb_d65)
    );
    print_vector(
        "stage.linear_srgb.edge_aware",
        apply_matrix(edge_balanced.values, transform.camera_to_linear_srgb_d65)
    );
}

[[nodiscard]] std::string usage() {
    return "usage: shadow-raw-stage-probe <input-raw> [--point <sensor-x> <sensor-y> | "
           "--sdk-point <sdk-x> <sdk-y> | --brightest]";
}

} // namespace

int main(const int argc, char* argv[]) {
    try {
        if (argc < 2) {
            throw std::runtime_error(usage());
        }
        std::optional<ProbeLocation> location;
        if (argc == 2) {
            location = std::nullopt;
        } else if (argc == 3 && std::string_view(argv[2]) == "--brightest") {
            location = std::nullopt;
        } else if (argc == 5 && std::string_view(argv[2]) == "--point") {
            location = ProbeLocation{
                .x = parse_coordinate(argv[3], "sensor x"),
                .y = parse_coordinate(argv[4], "sensor y"),
            };
        } else if (argc == 5 && std::string_view(argv[2]) == "--sdk-point") {
            location = ProbeLocation{
                .x = parse_coordinate(argv[3], "SDK x"),
                .y = parse_coordinate(argv[4], "SDK y"),
                .from_sdk_coordinates = true,
            };
        } else {
            throw std::runtime_error(usage());
        }
        trace(fs::path(argv[1]), location);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shadow-raw-stage-probe: " << error.what() << '\n';
        return 1;
    }
}
