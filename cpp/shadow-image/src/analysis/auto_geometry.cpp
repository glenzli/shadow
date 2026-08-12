#include <shadow/image/auto_geometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shadow::image {
namespace {

constexpr std::uint32_t maximum_analysis_edge = 512U;
constexpr std::uint32_t minimum_analysis_edge = 32U;
constexpr std::uint32_t maximum_source_edge = 8'192U;
constexpr std::size_t hough_angle_bins = 360U;
constexpr double degrees_to_radians = std::numbers::pi / 180.0;
constexpr double radians_to_degrees = 180.0 / std::numbers::pi;

struct Point final {
    double x = 0.0;
    double y = 0.0;
};

struct EdgeSample final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    double magnitude = 0.0;
    double normal_angle = 0.0;
};

struct DetectedLine final {
    Point first;
    Point second;
    double score = 0.0;
    double angle_radians = 0.0;
    double normal_angle = 0.0;
    double rho = 0.0;
    double midpoint_x = 0.0;
    double midpoint_y = 0.0;
    bool near_vertical = false;
    bool near_horizontal = false;
};

struct PerspectiveEstimate final {
    double amount = 0.0;
    double confidence = 0.0;
    std::uint32_t line_count = 0U;
};

[[nodiscard]] std::size_t checked_required_bytes(const AutoGeometryRgb8View source) {
    if (source.width == 0U || source.height == 0U || source.width > maximum_source_edge
        || source.height > maximum_source_edge) {
        throw std::invalid_argument("auto geometry requires bounded non-zero dimensions");
    }
    const std::size_t packed_row = static_cast<std::size_t>(source.width) * 3U;
    if (source.row_stride_bytes < packed_row) {
        throw std::invalid_argument("auto geometry RGB8 row stride is too small");
    }
    const std::size_t trailing_rows = static_cast<std::size_t>(source.height - 1U);
    if (trailing_rows > 0U
        && source.row_stride_bytes
               > (std::numeric_limits<std::size_t>::max() - packed_row) / trailing_rows) {
        throw std::invalid_argument("auto geometry RGB8 byte layout overflows");
    }
    const std::size_t required = source.row_stride_bytes * trailing_rows + packed_row;
    if (required > source.pixels.size()) {
        throw std::invalid_argument("auto geometry RGB8 payload is truncated");
    }
    return required;
}

[[nodiscard]] bool valid_mode(const AutoGeometryMode mode) noexcept {
    switch (mode) {
    case AutoGeometryMode::automatic:
    case AutoGeometryMode::level:
    case AutoGeometryMode::vertical:
    case AutoGeometryMode::full:
        return true;
    }
    return false;
}

[[nodiscard]] double normalized_half_turn(double angle) noexcept {
    angle = std::fmod(angle, std::numbers::pi);
    if (angle < 0.0) {
        angle += std::numbers::pi;
    }
    return angle;
}

[[nodiscard]] double display_luma(const std::uint8_t* const pixel) noexcept {
    return (0.2126 * static_cast<double>(pixel[0]) + 0.7152 * static_cast<double>(pixel[1])
            + 0.0722 * static_cast<double>(pixel[2]))
           / 255.0;
}

[[nodiscard]] std::vector<double> downsample_luma(
    const AutoGeometryRgb8View source,
    std::uint32_t& output_width,
    std::uint32_t& output_height
) {
    const double scale = std::min(
        1.0,
        static_cast<double>(maximum_analysis_edge)
            / static_cast<double>(std::max(source.width, source.height))
    );
    output_width = std::max(
        1U,
        static_cast<std::uint32_t>(std::floor(static_cast<double>(source.width) * scale))
    );
    output_height = std::max(
        1U,
        static_cast<std::uint32_t>(std::floor(static_cast<double>(source.height) * scale))
    );
    std::vector<double> result(static_cast<std::size_t>(output_width) * output_height, 0.0);
    const double source_per_output_x =
        static_cast<double>(source.width) / static_cast<double>(output_width);
    const double source_per_output_y =
        static_cast<double>(source.height) / static_cast<double>(output_height);
    for (std::uint32_t y = 0U; y < output_height; ++y) {
        const std::uint32_t source_y = std::min(
            source.height - 1U,
            static_cast<std::uint32_t>(
                std::floor((static_cast<double>(y) + 0.5) * source_per_output_y)
            )
        );
        const auto* const row =
            source.pixels.data() + static_cast<std::size_t>(source_y) * source.row_stride_bytes;
        for (std::uint32_t x = 0U; x < output_width; ++x) {
            const std::uint32_t source_x = std::min(
                source.width - 1U,
                static_cast<std::uint32_t>(
                    std::floor((static_cast<double>(x) + 0.5) * source_per_output_x)
                )
            );
            result[static_cast<std::size_t>(y) * output_width + x] =
                display_luma(row + static_cast<std::size_t>(source_x) * 3U);
        }
    }
    return result;
}

[[nodiscard]] std::vector<EdgeSample> detect_edges(
    const std::vector<double>& luma,
    const std::uint32_t width,
    const std::uint32_t height
) {
    std::vector<EdgeSample> candidates;
    std::vector<double> magnitudes;
    candidates.reserve(static_cast<std::size_t>(width) * height / 5U);
    magnitudes.reserve(candidates.capacity());
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y) {
        return luma[static_cast<std::size_t>(y) * width + x];
    };
    for (std::uint32_t y = 1U; y + 1U < height; ++y) {
        for (std::uint32_t x = 1U; x + 1U < width; ++x) {
            const double gx = -sample(x - 1U, y - 1U) + sample(x + 1U, y - 1U)
                              - 2.0 * sample(x - 1U, y) + 2.0 * sample(x + 1U, y)
                              - sample(x - 1U, y + 1U) + sample(x + 1U, y + 1U);
            const double gy = -sample(x - 1U, y - 1U) - 2.0 * sample(x, y - 1U)
                              - sample(x + 1U, y - 1U) + sample(x - 1U, y + 1U)
                              + 2.0 * sample(x, y + 1U) + sample(x + 1U, y + 1U);
            const double magnitude = std::hypot(gx, gy);
            if (magnitude < 0.03) {
                continue;
            }
            candidates.push_back({x, y, magnitude, normalized_half_turn(std::atan2(gy, gx))});
            magnitudes.push_back(magnitude);
        }
    }
    if (magnitudes.size() < 32U) {
        return {};
    }
    const std::size_t percentile_index = magnitudes.size() * 4U / 5U;
    std::nth_element(
        magnitudes.begin(),
        magnitudes.begin() + static_cast<std::ptrdiff_t>(percentile_index),
        magnitudes.end()
    );
    const double threshold = std::max(0.08, magnitudes[percentile_index]);
    candidates.erase(
        std::remove_if(
            candidates.begin(),
            candidates.end(),
            [threshold](const EdgeSample& edge) { return edge.magnitude < threshold; }
        ),
        candidates.end()
    );
    constexpr std::size_t maximum_edges = 80'000U;
    if (candidates.size() <= maximum_edges) {
        return candidates;
    }
    const std::size_t stride = (candidates.size() + maximum_edges - 1U) / maximum_edges;
    std::vector<EdgeSample> bounded;
    bounded.reserve(maximum_edges);
    for (std::size_t index = 0U; index < candidates.size(); index += stride) {
        bounded.push_back(candidates[index]);
    }
    return bounded;
}

[[nodiscard]] std::optional<std::pair<Point, Point>> clipped_line(
    const double rho,
    const double theta,
    const std::uint32_t width,
    const std::uint32_t height
) {
    const Point base{rho * std::cos(theta), rho * std::sin(theta)};
    const Point direction{-std::sin(theta), std::cos(theta)};
    const double half_width = (static_cast<double>(width) - 1.0) * 0.5;
    const double half_height = (static_cast<double>(height) - 1.0) * 0.5;
    double minimum_t = -std::numeric_limits<double>::infinity();
    double maximum_t = std::numeric_limits<double>::infinity();
    const auto clip_axis = [&](const double origin, const double delta, const double extent) {
        if (std::abs(delta) < 1.0e-12) {
            return origin >= -extent && origin <= extent;
        }
        double first = (-extent - origin) / delta;
        double second = (extent - origin) / delta;
        if (first > second) {
            std::swap(first, second);
        }
        minimum_t = std::max(minimum_t, first);
        maximum_t = std::min(maximum_t, second);
        return minimum_t <= maximum_t;
    };
    if (!clip_axis(base.x, direction.x, half_width)
        || !clip_axis(base.y, direction.y, half_height)) {
        return std::nullopt;
    }
    const auto normalized = [half_width, half_height](const Point value) {
        return Point{
            half_width > 0.0 ? value.x / half_width : 0.0,
            half_height > 0.0 ? value.y / half_height : 0.0,
        };
    };
    return std::pair{
        normalized({base.x + minimum_t * direction.x, base.y + minimum_t * direction.y}),
        normalized({base.x + maximum_t * direction.x, base.y + maximum_t * direction.y}),
    };
}

[[nodiscard]] std::vector<DetectedLine> detect_lines(
    const std::vector<EdgeSample>& edges,
    const std::uint32_t width,
    const std::uint32_t height
) {
    if (edges.empty()) {
        return {};
    }
    const double half_width = (static_cast<double>(width) - 1.0) * 0.5;
    const double half_height = (static_cast<double>(height) - 1.0) * 0.5;
    const int rho_extent = static_cast<int>(std::ceil(std::hypot(half_width, half_height))) + 2;
    const std::size_t rho_bins = static_cast<std::size_t>(rho_extent * 2 + 1);
    std::vector<double> accumulator(hough_angle_bins * rho_bins, 0.0);
    std::array<double, hough_angle_bins> cosines{};
    std::array<double, hough_angle_bins> sines{};
    for (std::size_t bin = 0U; bin < hough_angle_bins; ++bin) {
        const double theta =
            static_cast<double>(bin) * std::numbers::pi / static_cast<double>(hough_angle_bins);
        cosines[bin] = std::cos(theta);
        sines[bin] = std::sin(theta);
    }
    for (const auto& edge : edges) {
        const double centered_x = static_cast<double>(edge.x) - half_width;
        const double centered_y = static_cast<double>(edge.y) - half_height;
        const long center_bin = std::lround(
            edge.normal_angle / std::numbers::pi * static_cast<double>(hough_angle_bins)
        );
        // Raster stair-steps make a locally sampled Sobel normal noticeably
        // noisier than the supporting photographic line. Vote through a
        // bounded six-degree neighborhood; the global Hough peak, rather than
        // one pixel's quantized gradient, remains the orientation authority.
        constexpr int normal_vote_radius = 6;
        for (int offset = -normal_vote_radius; offset <= normal_vote_radius; ++offset) {
            const auto bin = static_cast<std::size_t>(
                (center_bin + offset + static_cast<long>(hough_angle_bins))
                % static_cast<long>(hough_angle_bins)
            );
            const int rho_bin =
                static_cast<int>(std::lround(centered_x * cosines[bin] + centered_y * sines[bin]))
                + rho_extent;
            if (rho_bin < 0 || static_cast<std::size_t>(rho_bin) >= rho_bins) {
                continue;
            }
            const double normalized_offset = static_cast<double>(offset) / 3.0;
            const double angular_weight = std::exp(-0.5 * normalized_offset * normalized_offset);
            accumulator[bin * rho_bins + static_cast<std::size_t>(rho_bin)] +=
                edge.magnitude * angular_weight;
        }
    }

    struct Peak final {
        std::size_t angle_bin = 0U;
        int rho = 0;
        double score = 0.0;
    };
    std::vector<Peak> peaks;
    const double minimum_score =
        std::max(4.0, static_cast<double>(std::min(width, height)) * 0.035);
    for (std::size_t angle_bin = 0U; angle_bin < hough_angle_bins; ++angle_bin) {
        for (std::size_t rho_bin = 0U; rho_bin < rho_bins; ++rho_bin) {
            const double score = accumulator[angle_bin * rho_bins + rho_bin];
            if (score >= minimum_score) {
                peaks.push_back({angle_bin, static_cast<int>(rho_bin) - rho_extent, score});
            }
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& left, const Peak& right) {
        return left.score > right.score;
    });

    std::vector<DetectedLine> lines;
    lines.reserve(64U);
    for (const auto& peak : peaks) {
        if (lines.size() >= 64U) {
            break;
        }
        const double theta = static_cast<double>(peak.angle_bin) * std::numbers::pi
                             / static_cast<double>(hough_angle_bins);
        const bool duplicate =
            std::any_of(lines.begin(), lines.end(), [&](const DetectedLine& line) {
                const double angular_distance =
                    std::abs(std::remainder(theta - line.normal_angle, std::numbers::pi));
                return angular_distance < 2.0 * degrees_to_radians
                       && std::abs(static_cast<double>(peak.rho) - line.rho) < 5.0;
            });
        if (duplicate) {
            continue;
        }
        const auto endpoints = clipped_line(static_cast<double>(peak.rho), theta, width, height);
        if (!endpoints.has_value()) {
            continue;
        }
        const double dx = (endpoints->second.x - endpoints->first.x) * half_width;
        const double dy = (endpoints->second.y - endpoints->first.y) * half_height;
        const double angle = std::atan2(dy, dx);
        lines.push_back({
            .first = endpoints->first,
            .second = endpoints->second,
            .score = peak.score,
            .angle_radians = angle,
            .normal_angle = theta,
            .rho = static_cast<double>(peak.rho),
            .midpoint_x = (endpoints->first.x + endpoints->second.x) * 0.5,
            .midpoint_y = (endpoints->first.y + endpoints->second.y) * 0.5,
            .near_vertical = std::abs(std::cos(angle)) <= 0.5,
            .near_horizontal = std::abs(std::sin(angle)) <= 0.5,
        });
    }
    return lines;
}

[[nodiscard]] double weighted_median(std::vector<std::pair<double, double>> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    double total = 0.0;
    for (const auto& entry : values) {
        total += entry.second;
    }
    double cumulative = 0.0;
    for (const auto& entry : values) {
        cumulative += entry.second;
        if (cumulative * 2.0 >= total) {
            return entry.first;
        }
    }
    return values.back().first;
}

[[nodiscard]] double axis_residual(double angle) noexcept {
    angle = std::remainder(angle, std::numbers::pi / 2.0);
    return std::clamp(angle, -std::numbers::pi / 4.0, std::numbers::pi / 4.0);
}

[[nodiscard]] std::pair<double, double>
estimate_straighten(const std::vector<DetectedLine>& lines) {
    std::vector<std::pair<double, double>> residuals;
    for (const auto& line : lines) {
        if (line.near_horizontal || line.near_vertical) {
            residuals.emplace_back(axis_residual(line.angle_radians), line.score);
        }
    }
    if (residuals.empty()) {
        return {0.0, 0.0};
    }
    constexpr double minimum_degrees = -30.0;
    constexpr double bin_degrees = 0.25;
    constexpr std::size_t bin_count = 241U;
    std::array<double, bin_count> orientation_support{};
    double total_weight = 0.0;
    for (const auto& entry : residuals) {
        const double degrees = entry.first * radians_to_degrees;
        const long center = std::lround((degrees - minimum_degrees) / bin_degrees);
        total_weight += entry.second;
        for (int offset = -4; offset <= 4; ++offset) {
            const long bin = center + offset;
            if (bin < 0 || bin >= static_cast<long>(bin_count)) {
                continue;
            }
            const double normalized = static_cast<double>(offset) / 2.0;
            orientation_support[static_cast<std::size_t>(bin)] +=
                entry.second * std::exp(-0.5 * normalized * normalized);
        }
    }
    const auto best = std::max_element(orientation_support.begin(), orientation_support.end());
    const double dominant_degrees =
        minimum_degrees
        + static_cast<double>(std::distance(orientation_support.begin(), best)) * bin_degrees;
    std::vector<std::pair<double, double>> dominant;
    double dominant_weight = 0.0;
    for (const auto& entry : residuals) {
        if (std::abs(entry.first * radians_to_degrees - dominant_degrees) <= 2.0) {
            dominant.push_back(entry);
            dominant_weight += entry.second;
        }
    }
    if (dominant.empty()) {
        return {0.0, 0.0};
    }
    const double median = weighted_median(dominant);
    std::vector<std::pair<double, double>> deviations;
    deviations.reserve(dominant.size());
    for (const auto& entry : dominant) {
        deviations.emplace_back(std::abs(entry.first - median), entry.second);
    }
    const double mad = weighted_median(std::move(deviations));
    double correction = std::clamp(-median * radians_to_degrees, -15.0, 15.0);
    if (std::abs(correction) < 0.15) {
        correction = 0.0;
    }
    const double consistency = std::clamp(1.0 - mad / (8.0 * degrees_to_radians), 0.0, 1.0);
    const double count_support = std::clamp(static_cast<double>(dominant.size()) / 5.0, 0.0, 1.0);
    const double weight_support =
        total_weight > 0.0 ? std::clamp(dominant_weight / total_weight * 2.0, 0.0, 1.0) : 0.0;
    return {correction, consistency * count_support * weight_support};
}

struct PerspectiveCoefficients final {
    double c = 0.0;
    double k = 1.0;
};

[[nodiscard]] PerspectiveCoefficients perspective_coefficients(const double amount) noexcept {
    const double reduced = 1.0 - 0.5 * std::abs(amount);
    const double first = amount >= 0.0 ? reduced : 1.0;
    const double second = amount >= 0.0 ? 1.0 : reduced;
    return {
        .c = (first - second) / (first + second),
        .k = 2.0 * first * second / (first + second),
    };
}

[[nodiscard]] Point
inverse_perspective(Point point, const double vertical, const double horizontal) noexcept {
    const auto horizontal_coefficients = perspective_coefficients(horizontal);
    const double horizontal_denominator = 1.0 - horizontal_coefficients.c * point.x;
    if (std::abs(horizontal_denominator) < 1.0e-9) {
        return {1000.0, 1000.0};
    }
    const double horizontal_x = (point.x - horizontal_coefficients.c) / horizontal_denominator;
    const double horizontal_y =
        point.y * (1.0 + horizontal_coefficients.c * horizontal_x) / horizontal_coefficients.k;
    const auto vertical_coefficients = perspective_coefficients(vertical);
    const double vertical_denominator = 1.0 - vertical_coefficients.c * horizontal_y;
    if (std::abs(vertical_denominator) < 1.0e-9) {
        return {1000.0, 1000.0};
    }
    const double vertical_y = (horizontal_y - vertical_coefficients.c) / vertical_denominator;
    const double vertical_x =
        horizontal_x * (1.0 + vertical_coefficients.c * vertical_y) / vertical_coefficients.k;
    return {vertical_x, vertical_y};
}

[[nodiscard]] Point proposed_output_point(
    const Point source,
    const double straighten_degrees,
    const double vertical,
    const double horizontal,
    const double pixel_scale_x,
    const double pixel_scale_y
) noexcept {
    const Point unprojected = inverse_perspective(source, vertical, horizontal);
    const double angle = straighten_degrees * degrees_to_radians;
    return {
        std::cos(angle) * unprojected.x * pixel_scale_x
            - std::sin(angle) * unprojected.y * pixel_scale_y,
        std::sin(angle) * unprojected.x * pixel_scale_x
            + std::cos(angle) * unprojected.y * pixel_scale_y,
    };
}

[[nodiscard]] double line_axis_cost(
    const std::vector<DetectedLine>& lines,
    const bool vertical_axis,
    const double straighten_degrees,
    const double vertical,
    const double horizontal,
    const double pixel_scale_x,
    const double pixel_scale_y
) {
    double weighted_cost = 0.0;
    double total_weight = 0.0;
    for (const auto& line : lines) {
        if ((vertical_axis && !line.near_vertical) || (!vertical_axis && !line.near_horizontal)) {
            continue;
        }
        const Point first = proposed_output_point(
            line.first,
            straighten_degrees,
            vertical,
            horizontal,
            pixel_scale_x,
            pixel_scale_y
        );
        const Point second = proposed_output_point(
            line.second,
            straighten_degrees,
            vertical,
            horizontal,
            pixel_scale_x,
            pixel_scale_y
        );
        const double dx = second.x - first.x;
        const double dy = second.y - first.y;
        const double length_squared = dx * dx + dy * dy;
        if (length_squared < 1.0e-8) {
            continue;
        }
        const double residual = vertical_axis ? dx * dx / length_squared : dy * dy / length_squared;
        weighted_cost += line.score * std::min(residual, 0.25);
        total_weight += line.score;
    }
    return total_weight > 0.0 ? weighted_cost / total_weight
                              : std::numeric_limits<double>::infinity();
}

[[nodiscard]] PerspectiveEstimate estimate_perspective(
    const std::vector<DetectedLine>& lines,
    const bool vertical_axis,
    const double straighten_degrees,
    const bool permissive,
    const std::uint32_t width,
    const std::uint32_t height
) {
    std::vector<double> positions;
    for (const auto& line : lines) {
        if ((vertical_axis && line.near_vertical) || (!vertical_axis && line.near_horizontal)) {
            positions.push_back(vertical_axis ? line.midpoint_x : line.midpoint_y);
        }
    }
    if (positions.size() < 2U) {
        return {};
    }
    const auto [minimum, maximum] = std::minmax_element(positions.begin(), positions.end());
    if (*maximum - *minimum < 0.35) {
        return {};
    }
    const double pixel_scale_x = (static_cast<double>(width) - 1.0) * 0.5;
    const double pixel_scale_y = (static_cast<double>(height) - 1.0) * 0.5;
    const double baseline = line_axis_cost(
        lines,
        vertical_axis,
        straighten_degrees,
        0.0,
        0.0,
        pixel_scale_x,
        pixel_scale_y
    );
    if (!std::isfinite(baseline) || baseline < 1.0e-7) {
        return {};
    }
    double best_amount = 0.0;
    double best_cost = baseline;
    for (int step = -80; step <= 80; ++step) {
        const double amount = static_cast<double>(step) / 100.0;
        const double cost = line_axis_cost(
                                lines,
                                vertical_axis,
                                straighten_degrees,
                                vertical_axis ? amount : 0.0,
                                vertical_axis ? 0.0 : amount,
                                pixel_scale_x,
                                pixel_scale_y
                            )
                            + amount * amount * 2.0e-5;
        if (cost < best_cost) {
            best_cost = cost;
            best_amount = amount;
        }
    }
    const double improvement = std::clamp((baseline - best_cost) / baseline, 0.0, 1.0);
    if (std::abs(best_amount) < 0.025 || improvement < (permissive ? 0.08 : 0.14)) {
        return {};
    }
    const double support = std::clamp(static_cast<double>(positions.size()) / 6.0, 0.0, 1.0);
    const double spread = std::clamp((*maximum - *minimum) / 1.25, 0.0, 1.0);
    return {
        best_amount,
        improvement * support * spread,
        static_cast<std::uint32_t>(positions.size())
    };
}

} // namespace

AutoGeometryProposal
analyze_auto_geometry(const AutoGeometryRgb8View source, const AutoGeometryMode mode) {
    static_cast<void>(checked_required_bytes(source));
    if (!valid_mode(mode)) {
        throw std::invalid_argument("auto geometry mode is unsupported");
    }
    AutoGeometryProposal proposal{.mode = mode};
    if (std::min(source.width, source.height) < minimum_analysis_edge) {
        return proposal;
    }
    std::uint32_t analysis_width = 0U;
    std::uint32_t analysis_height = 0U;
    const auto luma = downsample_luma(source, analysis_width, analysis_height);
    const auto edges = detect_edges(luma, analysis_width, analysis_height);
    const auto lines = detect_lines(edges, analysis_width, analysis_height);
    const auto [straighten, level_confidence] = estimate_straighten(lines);
    const bool wants_vertical = mode == AutoGeometryMode::automatic
                                || mode == AutoGeometryMode::vertical
                                || mode == AutoGeometryMode::full;
    const bool wants_horizontal =
        mode == AutoGeometryMode::automatic || mode == AutoGeometryMode::full;
    const bool permissive = mode == AutoGeometryMode::full;
    const auto vertical = wants_vertical ? estimate_perspective(
                                               lines,
                                               true,
                                               straighten,
                                               permissive,
                                               analysis_width,
                                               analysis_height
                                           )
                                         : PerspectiveEstimate{};
    const auto horizontal = wants_horizontal ? estimate_perspective(
                                                   lines,
                                                   false,
                                                   straighten,
                                                   permissive,
                                                   analysis_width,
                                                   analysis_height
                                               )
                                             : PerspectiveEstimate{};
    proposal.straighten_degrees = straighten;
    proposal.perspective_vertical = vertical.amount;
    proposal.perspective_horizontal = horizontal.amount;
    proposal.vertical_lines = vertical.line_count;
    proposal.horizontal_lines = horizontal.line_count;
    proposal.supporting_lines = static_cast<std::uint32_t>(
        std::count_if(lines.begin(), lines.end(), [](const DetectedLine& line) {
            return line.near_vertical || line.near_horizontal;
        })
    );
    proposal.confidence = std::clamp(
        std::max({level_confidence, vertical.confidence, horizontal.confidence}),
        0.0,
        1.0
    );
    const bool meaningful_level = std::abs(straighten) >= 0.15 && level_confidence >= 0.15;
    const bool meaningful_vertical =
        std::abs(vertical.amount) >= 0.025 && vertical.confidence >= (permissive ? 0.08 : 0.12);
    const bool meaningful_horizontal =
        std::abs(horizontal.amount) >= 0.025 && horizontal.confidence >= (permissive ? 0.08 : 0.12);
    proposal.available = meaningful_level || meaningful_vertical || meaningful_horizontal;
    if (!proposal.available) {
        proposal.straighten_degrees = 0.0;
        proposal.perspective_vertical = 0.0;
        proposal.perspective_horizontal = 0.0;
    }
    return proposal;
}

} // namespace shadow::image
