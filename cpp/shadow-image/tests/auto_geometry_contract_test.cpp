#include <shadow/image/auto_geometry.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t fixture_width = 480U;
constexpr std::uint32_t fixture_height = 320U;

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

void set_pixel(
    std::vector<std::uint8_t>& pixels,
    const int x,
    const int y,
    const std::uint8_t value = 255U
) {
    if (x < 0 || y < 0 || x >= static_cast<int>(fixture_width)
        || y >= static_cast<int>(fixture_height)) {
        return;
    }
    const std::size_t offset =
        (static_cast<std::size_t>(y) * fixture_width + static_cast<std::size_t>(x)) * 3U;
    pixels[offset] = value;
    pixels[offset + 1U] = value;
    pixels[offset + 2U] = value;
}

void draw_line(
    std::vector<std::uint8_t>& pixels,
    const int x0,
    const int y0,
    const int x1,
    const int y1
) {
    const double dx = static_cast<double>(x1 - x0);
    const double dy = static_cast<double>(y1 - y0);
    const double length_squared = dx * dx + dy * dy;
    for (int y = 0; y < static_cast<int>(fixture_height); ++y) {
        for (int x = 0; x < static_cast<int>(fixture_width); ++x) {
            const double projection = std::clamp(
                ((static_cast<double>(x - x0) * dx) + (static_cast<double>(y - y0) * dy))
                    / length_squared,
                0.0,
                1.0
            );
            const double nearest_x = static_cast<double>(x0) + projection * dx;
            const double nearest_y = static_cast<double>(y0) + projection * dy;
            const double distance =
                std::hypot(static_cast<double>(x) - nearest_x, static_cast<double>(y) - nearest_y);
            if (distance <= 1.75) {
                const auto value = static_cast<std::uint8_t>(
                    std::lround(48.0 + std::clamp(1.75 - distance, 0.0, 1.0) * 207.0)
                );
                set_pixel(pixels, x, y, value);
            }
        }
    }
}

[[nodiscard]] shadow::image::AutoGeometryRgb8View view(const std::vector<std::uint8_t>& pixels) {
    return {
        .pixels = pixels,
        .width = fixture_width,
        .height = fixture_height,
        .row_stride_bytes = static_cast<std::size_t>(fixture_width) * 3U,
    };
}

[[nodiscard]] std::vector<std::uint8_t> tilted_grid(const double degrees) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(fixture_width) * fixture_height * 3U,
        16U
    );
    const double slope = std::tan(degrees * 3.14159265358979323846 / 180.0);
    for (int y = 55; y <= 265; y += 42) {
        draw_line(
            pixels,
            20,
            y - static_cast<int>(std::lround(slope * 220.0)),
            459,
            y + static_cast<int>(std::lround(slope * 219.0))
        );
    }
    for (int x = 70; x <= 410; x += 68) {
        draw_line(
            pixels,
            x + static_cast<int>(std::lround(slope * 130.0)),
            30,
            x - static_cast<int>(std::lround(slope * 159.0)),
            289
        );
    }
    return pixels;
}

[[nodiscard]] std::vector<std::uint8_t> converging_verticals() {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(fixture_width) * fixture_height * 3U,
        12U
    );
    for (int bottom_x = 50; bottom_x <= 430; bottom_x += 76) {
        const int top_x = 240 + (bottom_x - 240) * 3 / 5;
        draw_line(pixels, top_x, 18, bottom_x, 301);
    }
    draw_line(pixels, 20, 70, 459, 70);
    draw_line(pixels, 20, 250, 459, 250);
    return pixels;
}

[[nodiscard]] std::vector<std::uint8_t> converging_horizontals() {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(fixture_width) * fixture_height * 3U,
        12U
    );
    for (int right_y = 35; right_y <= 285; right_y += 50) {
        const int left_y = 160 + (right_y - 160) * 3 / 5;
        draw_line(pixels, 18, left_y, 461, right_y);
    }
    draw_line(pixels, 90, 18, 90, 301);
    draw_line(pixels, 390, 18, 390, 301);
    return pixels;
}

bool level_proposal_corrects_a_tilted_grid() {
    const auto pixels = tilted_grid(5.0);
    const auto proposal =
        shadow::image::analyze_auto_geometry(view(pixels), shadow::image::AutoGeometryMode::level);
    if (!proposal.available) {
        std::cerr << "level diagnostic: angle=" << proposal.straighten_degrees
                  << " confidence=" << proposal.confidence << " lines=" << proposal.supporting_lines
                  << '\n';
    }
    if (!(proposal.straighten_degrees < -3.5 && proposal.straighten_degrees > -6.5)) {
        std::cerr << "level angle diagnostic: " << proposal.straighten_degrees << '\n';
    }
    return require(proposal.available, "tilted grid produces a proposal")
           && require(proposal.supporting_lines >= 4U, "tilted grid retains line evidence")
           && require(
               proposal.straighten_degrees < -3.5 && proposal.straighten_degrees > -6.5,
               "level proposal reverses the observed five-degree tilt"
           )
           && require(
               proposal.perspective_vertical == 0.0 && proposal.perspective_horizontal == 0.0,
               "level mode never authors perspective"
           );
}

bool vertical_mode_detects_convergence() {
    const auto pixels = converging_verticals();
    const auto proposal = shadow::image::analyze_auto_geometry(
        view(pixels),
        shadow::image::AutoGeometryMode::vertical
    );
    if (!proposal.available) {
        std::cerr << "vertical diagnostic: angle=" << proposal.straighten_degrees
                  << " vertical=" << proposal.perspective_vertical
                  << " confidence=" << proposal.confidence << " lines=" << proposal.supporting_lines
                  << " vertical-lines=" << proposal.vertical_lines << '\n';
    }
    return require(proposal.available, "converging verticals produce a proposal")
           && require(
               proposal.vertical_lines >= 2U,
               "vertical proposal retains distributed evidence"
           )
           && require(
               std::abs(proposal.perspective_vertical) >= 0.08,
               "vertical proposal contains a material keystone correction"
           )
           && require(
               proposal.perspective_horizontal == 0.0,
               "vertical mode never authors horizontal perspective"
           );
}

bool full_mode_detects_horizontal_convergence() {
    const auto pixels = converging_horizontals();
    const auto proposal =
        shadow::image::analyze_auto_geometry(view(pixels), shadow::image::AutoGeometryMode::full);
    if (!proposal.available) {
        std::cerr << "horizontal diagnostic: angle=" << proposal.straighten_degrees
                  << " horizontal=" << proposal.perspective_horizontal
                  << " confidence=" << proposal.confidence << " lines=" << proposal.supporting_lines
                  << " horizontal-lines=" << proposal.horizontal_lines << '\n';
    }
    return require(proposal.available, "converging horizontals produce a proposal")
           && require(
               proposal.horizontal_lines >= 2U,
               "full proposal retains distributed horizontal evidence"
           )
           && require(
               std::abs(proposal.perspective_horizontal) >= 0.08,
               "full proposal contains a material horizontal keystone correction"
           );
}

bool inconclusive_and_invalid_inputs_fail_closed() {
    std::vector<std::uint8_t> blank(
        static_cast<std::size_t>(fixture_width) * fixture_height * 3U,
        80U
    );
    const auto blank_proposal = shadow::image::analyze_auto_geometry(view(blank));
    if (!require(!blank_proposal.available, "blank raster has no proposal")) {
        return false;
    }
    bool truncated_rejected = false;
    try {
        static_cast<void>(shadow::image::analyze_auto_geometry({
            .pixels = std::span<const std::uint8_t>(blank.data(), 16U),
            .width = fixture_width,
            .height = fixture_height,
            .row_stride_bytes = static_cast<std::size_t>(fixture_width) * 3U,
        }));
    } catch (const std::invalid_argument&) {
        truncated_rejected = true;
    }
    bool overflow_rejected = false;
    try {
        static_cast<void>(shadow::image::analyze_auto_geometry({
            .pixels = blank,
            .width = fixture_width,
            .height = fixture_height,
            .row_stride_bytes = std::numeric_limits<std::size_t>::max(),
        }));
    } catch (const std::invalid_argument&) {
        overflow_rejected = true;
    }
    bool invalid_mode_rejected = false;
    try {
        static_cast<void>(shadow::image::analyze_auto_geometry(
            view(blank),
            static_cast<shadow::image::AutoGeometryMode>(255U)
        ));
    } catch (const std::invalid_argument&) {
        invalid_mode_rejected = true;
    }
    return require(truncated_rejected, "truncated RGB8 payload is rejected")
           && require(overflow_rejected, "overflowing RGB8 byte layout is rejected")
           && require(invalid_mode_rejected, "unsupported analysis mode is rejected");
}

} // namespace

int main() {
    return level_proposal_corrects_a_tilted_grid() && vertical_mode_detects_convergence()
                   && full_mode_detects_horizontal_convergence()
                   && inconclusive_and_invalid_inputs_fail_closed()
               ? 0
               : 1;
}
