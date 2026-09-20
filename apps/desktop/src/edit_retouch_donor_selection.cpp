#include "edit_retouch_donor_selection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <vector>

namespace {

constexpr std::size_t RGB_CHANNELS = 3U;
constexpr std::size_t MAX_PATH_SAMPLES = 32U;

struct Sample final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

struct Gradient final {
    double horizontal = 0.0;
    double vertical = 0.0;
};

[[nodiscard]] bool valid_request(const EditRetouchDonorRequest& request) noexcept {
    if (!request.preview_dimensions.isValid() || request.preview_dimensions.isEmpty()
        || !request.level_zero_dimensions.isValid() || request.level_zero_dimensions.isEmpty()
        || request.normalized_target_points.empty()
        || !std::isfinite(request.radius_level_zero_pixels)
        || request.radius_level_zero_pixels <= 0.0) {
        return false;
    }
    const auto width = static_cast<std::size_t>(request.preview_dimensions.width());
    const auto height = static_cast<std::size_t>(request.preview_dimensions.height());
    if (width > std::numeric_limits<std::size_t>::max() / RGB_CHANNELS) {
        return false;
    }
    const std::size_t packed_stride = width * RGB_CHANNELS;
    if (request.preview_row_stride_bytes < packed_stride
        || height > std::numeric_limits<std::size_t>::max() / request.preview_row_stride_bytes) {
        return false;
    }
    return request.preview_rgb8.size() >= height * request.preview_row_stride_bytes;
}

[[nodiscard]] Sample
sample_rgb8(const EditRetouchDonorRequest& request, const double x, const double y) noexcept {
    const int width = request.preview_dimensions.width();
    const int height = request.preview_dimensions.height();
    const double clamped_x = std::clamp(x, 0.0, static_cast<double>(width - 1));
    const double clamped_y = std::clamp(y, 0.0, static_cast<double>(height - 1));
    const int x0 = static_cast<int>(std::floor(clamped_x));
    const int y0 = static_cast<int>(std::floor(clamped_y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double blend_x = clamped_x - static_cast<double>(x0);
    const double blend_y = clamped_y - static_cast<double>(y0);
    constexpr double inverse_byte = 1.0 / 255.0;
    const auto channel = [&](const int sample_x, const int sample_y, const std::size_t offset) {
        const std::size_t index =
            static_cast<std::size_t>(sample_y) * request.preview_row_stride_bytes
            + static_cast<std::size_t>(sample_x) * RGB_CHANNELS + offset;
        return static_cast<double>(request.preview_rgb8[index]) * inverse_byte;
    };
    const auto interpolate = [&](const std::size_t offset) {
        const double top = std::lerp(channel(x0, y0, offset), channel(x1, y0, offset), blend_x);
        const double bottom = std::lerp(channel(x0, y1, offset), channel(x1, y1, offset), blend_x);
        return std::lerp(top, bottom, blend_y);
    };
    return {.red = interpolate(0U), .green = interpolate(1U), .blue = interpolate(2U)};
}

[[nodiscard]] double color_distance(const Sample first, const Sample second) noexcept {
    const double red = first.red - second.red;
    const double green = first.green - second.green;
    const double blue = first.blue - second.blue;
    return std::fma(red, red, std::fma(green, green, blue * blue));
}

[[nodiscard]] double luma(const Sample sample) noexcept {
    return 0.2126 * sample.red + 0.7152 * sample.green + 0.0722 * sample.blue;
}

[[nodiscard]] bool sample_fits(
    const EditRetouchDonorRequest& request,
    const double x,
    const double y,
    const double margin
) noexcept {
    return x >= margin && y >= margin
           && x <= static_cast<double>(request.preview_dimensions.width() - 1) - margin
           && y <= static_cast<double>(request.preview_dimensions.height() - 1) - margin;
}

[[nodiscard]] Gradient gradient(
    const EditRetouchDonorRequest& request,
    const double x,
    const double y,
    const double step
) noexcept {
    return {
        .horizontal =
            luma(sample_rgb8(request, x + step, y)) - luma(sample_rgb8(request, x - step, y)),
        .vertical =
            luma(sample_rgb8(request, x, y + step)) - luma(sample_rgb8(request, x, y - step)),
    };
}

[[nodiscard]] std::vector<QPointF>
sampled_path(const std::span<const QPointF> points, const double width, const double height) {
    const std::size_t count = std::min(points.size(), MAX_PATH_SAMPLES);
    std::vector<QPointF> result;
    result.reserve(count);
    for (std::size_t sample = 0U; sample < count; ++sample) {
        const std::size_t index = count == 1U ? 0U : sample * (points.size() - 1U) / (count - 1U);
        const QPointF point = points[index];
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || point.x() < 0.0
            || point.x() > 1.0 || point.y() < 0.0 || point.y() > 1.0) {
            return {};
        }
        result.emplace_back(point.x() * (width - 1.0), point.y() * (height - 1.0));
    }
    return result;
}

[[nodiscard]] bool source_fits(
    const std::vector<QPointF>& path,
    const double offset_x,
    const double offset_y,
    const double radius_x,
    const double radius_y,
    const double width,
    const double height
) noexcept {
    const double margin_x = radius_x * 1.18 + 2.0;
    const double margin_y = radius_y * 1.18 + 2.0;
    return std::ranges::all_of(path, [&](const QPointF point) {
        const double source_x = point.x() + offset_x;
        const double source_y = point.y() + offset_y;
        return source_x >= margin_x && source_x <= width - 1.0 - margin_x && source_y >= margin_y
               && source_y <= height - 1.0 - margin_y;
    });
}

[[nodiscard]] bool source_is_separate(
    const std::vector<QPointF>& path,
    const double offset_x,
    const double offset_y,
    const double radius_x,
    const double radius_y
) noexcept {
    constexpr double minimum_center_distance_radii = 2.15;
    const double minimum_distance_squared =
        minimum_center_distance_radii * minimum_center_distance_radii;
    const QPointF normalized_offset(offset_x / radius_x, offset_y / radius_y);
    if (path.size() == 1U) {
        return std::fma(
                   normalized_offset.x(),
                   normalized_offset.x(),
                   normalized_offset.y() * normalized_offset.y()
               )
               >= minimum_distance_squared;
    }
    const auto normalized = [&](const QPointF point) {
        return QPointF(point.x() / radius_x, point.y() / radius_y);
    };
    const auto cross = [](const QPointF first, const QPointF second) {
        return first.x() * second.y() - first.y() * second.x();
    };
    const auto point_segment_distance_squared = [&](const QPointF point,
                                                    const QPointF start,
                                                    const QPointF end) {
        const QPointF segment = end - start;
        const double length_squared = std::fma(segment.x(), segment.x(), segment.y() * segment.y());
        const double progress =
            length_squared > 1.0e-12
                ? std::clamp(QPointF::dotProduct(point - start, segment) / length_squared, 0.0, 1.0)
                : 0.0;
        const QPointF delta = point - (start + progress * segment);
        return std::fma(delta.x(), delta.x(), delta.y() * delta.y());
    };
    const auto segment_distance_squared = [&](const QPointF first_start,
                                              const QPointF first_end,
                                              const QPointF second_start,
                                              const QPointF second_end) {
        const QPointF first_direction = first_end - first_start;
        const QPointF second_direction = second_end - second_start;
        const double first_side_start = cross(first_direction, second_start - first_start);
        const double first_side_end = cross(first_direction, second_end - first_start);
        const double second_side_start = cross(second_direction, first_start - second_start);
        const double second_side_end = cross(second_direction, first_end - second_start);
        constexpr double intersection_epsilon = 1.0e-10;
        const auto opposite_sides = [=](const double first, const double second) {
            return (first > intersection_epsilon && second < -intersection_epsilon)
                   || (first < -intersection_epsilon && second > intersection_epsilon);
        };
        const auto on_segment =
            [=](const QPointF point, const QPointF start, const QPointF end, const double side) {
                return std::abs(side) <= intersection_epsilon
                       && point.x() >= std::min(start.x(), end.x()) - intersection_epsilon
                       && point.x() <= std::max(start.x(), end.x()) + intersection_epsilon
                       && point.y() >= std::min(start.y(), end.y()) - intersection_epsilon
                       && point.y() <= std::max(start.y(), end.y()) + intersection_epsilon;
            };
        if ((opposite_sides(first_side_start, first_side_end)
             && opposite_sides(second_side_start, second_side_end))
            || on_segment(second_start, first_start, first_end, first_side_start)
            || on_segment(second_end, first_start, first_end, first_side_end)
            || on_segment(first_start, second_start, second_end, second_side_start)
            || on_segment(first_end, second_start, second_end, second_side_end)) {
            return 0.0;
        }
        return std::min({
            point_segment_distance_squared(first_start, second_start, second_end),
            point_segment_distance_squared(first_end, second_start, second_end),
            point_segment_distance_squared(second_start, first_start, first_end),
            point_segment_distance_squared(second_end, first_start, first_end),
        });
    };
    for (std::size_t target = 1U; target < path.size(); ++target) {
        const QPointF target_start = normalized(path[target - 1U]);
        const QPointF target_end = normalized(path[target]);
        for (std::size_t source = 1U; source < path.size(); ++source) {
            const QPointF source_start = normalized(path[source - 1U]) + normalized_offset;
            const QPointF source_end = normalized(path[source]) + normalized_offset;
            if (segment_distance_squared(target_start, target_end, source_start, source_end)
                < minimum_distance_squared) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] double candidate_score(
    const EditRetouchDonorRequest& request,
    const std::vector<QPointF>& path,
    const double offset_x,
    const double offset_y,
    const double radius_x,
    const double radius_y
) noexcept {
    constexpr std::array<double, 8U> angles{
        0.0,
        std::numbers::pi / 4.0,
        std::numbers::pi / 2.0,
        3.0 * std::numbers::pi / 4.0,
        std::numbers::pi,
        5.0 * std::numbers::pi / 4.0,
        3.0 * std::numbers::pi / 2.0,
        7.0 * std::numbers::pi / 4.0,
    };
    constexpr std::array<double, 3U> scales{1.12, 1.75, 2.45};
    const double color_weight = request.mode == EditRetouchDonorMode::Clone ? 1.0 : 0.28;
    double score = 0.0;
    std::size_t sample_count = 0U;
    std::size_t target_sample_count = 0U;
    for (const QPointF point : path) {
        for (const double scale : scales) {
            const double gradient_step = std::max(1.0, std::min(radius_x, radius_y) * scale * 0.18);
            for (const double angle : angles) {
                const double ring_x = std::cos(angle) * radius_x * scale;
                const double ring_y = std::sin(angle) * radius_y * scale;
                const double target_x = point.x() + ring_x;
                const double target_y = point.y() + ring_y;
                if (!sample_fits(request, target_x, target_y, gradient_step)) {
                    continue;
                }
                ++target_sample_count;
                const double source_x = target_x + offset_x;
                const double source_y = target_y + offset_y;
                if (!sample_fits(request, source_x, source_y, gradient_step)) {
                    continue;
                }
                const Sample target = sample_rgb8(request, target_x, target_y);
                const Sample source = sample_rgb8(request, source_x, source_y);
                const double scale_weight = 1.0 / scale;
                score += color_weight * scale_weight * color_distance(target, source);
                const Gradient target_gradient =
                    gradient(request, target_x, target_y, gradient_step);
                const Gradient source_gradient =
                    gradient(request, source_x, source_y, gradient_step);
                const double horizontal_delta =
                    target_gradient.horizontal - source_gradient.horizontal;
                const double vertical_delta = target_gradient.vertical - source_gradient.vertical;
                score +=
                    1.8 * scale_weight
                    * std::fma(horizontal_delta, horizontal_delta, vertical_delta * vertical_delta);
                ++sample_count;
            }
        }
    }
    if (sample_count == 0U || sample_count * 4U < target_sample_count * 3U) {
        return std::numeric_limits<double>::infinity();
    }
    return score / static_cast<double>(sample_count);
}

} // namespace

std::optional<EditRetouchDonorSelection>
select_edit_retouch_donor(const EditRetouchDonorRequest& request) {
    if (!valid_request(request)) {
        return std::nullopt;
    }
    const double preview_width = static_cast<double>(request.preview_dimensions.width());
    const double preview_height = static_cast<double>(request.preview_dimensions.height());
    const double radius_x = request.radius_level_zero_pixels * preview_width
                            / static_cast<double>(request.level_zero_dimensions.width());
    const double radius_y = request.radius_level_zero_pixels * preview_height
                            / static_cast<double>(request.level_zero_dimensions.height());
    if (!std::isfinite(radius_x) || !std::isfinite(radius_y) || radius_x < 0.25
        || radius_y < 0.25) {
        return std::nullopt;
    }
    const std::vector<QPointF> path =
        sampled_path(request.normalized_target_points, preview_width, preview_height);
    if (path.empty()) {
        return std::nullopt;
    }

    constexpr std::array<double, 8U> distances{2.5, 3.25, 4.0, 5.0, 6.25, 7.5, 9.0, 10.5};
    constexpr std::size_t direction_count = 16U;
    const double maximum_offset_radii = (512.0 - 1.0) / request.radius_level_zero_pixels - 1.0;
    double best_score = std::numeric_limits<double>::infinity();
    double second_best_score = std::numeric_limits<double>::infinity();
    std::optional<QPointF> best;
    std::size_t candidate_count = 0U;
    std::vector<std::pair<double, QPointF>> ranked;
    for (const double distance : distances) {
        if (distance > maximum_offset_radii) {
            continue;
        }
        for (std::size_t direction = 0U; direction < direction_count; ++direction) {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(direction)
                                 / static_cast<double>(direction_count);
            const double offset_x_radii = distance * std::cos(angle);
            const double offset_y_radii = distance * std::sin(angle);
            const double offset_x = offset_x_radii * radius_x;
            const double offset_y = offset_y_radii * radius_y;
            if (!source_fits(
                    path,
                    offset_x,
                    offset_y,
                    radius_x,
                    radius_y,
                    preview_width,
                    preview_height
                )) {
                continue;
            }
            if (!source_is_separate(path, offset_x, offset_y, radius_x, radius_y)) {
                continue;
            }
            const double score =
                candidate_score(request, path, offset_x, offset_y, radius_x, radius_y);
            if (!std::isfinite(score)) {
                continue;
            }
            ++candidate_count;
            ranked.emplace_back(score, QPointF(offset_x_radii, offset_y_radii));
            if (score < best_score) {
                second_best_score = best_score;
                best_score = score;
                best = QPointF(offset_x_radii, offset_y_radii);
            } else if (score < second_best_score) {
                second_best_score = score;
            }
        }
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    // Boundary similarity is the primary signal. Candidate separation is a
    // smaller tiebreaker so an even sky or wall remains a useful, confident
    // automatic source instead of being rejected merely because many nearby
    // patches are equivalent.
    const double absolute_quality = 1.0 / (1.0 + 20.0 * best_score);
    const double separation =
        std::isfinite(second_best_score) && second_best_score > 1.0e-12
            ? std::clamp((second_best_score - best_score) / second_best_score, 0.0, 1.0)
            : 0.0;
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    std::vector<QPointF> alternatives;
    for (const auto& [score, offset] : ranked) {
        if (std::none_of(alternatives.begin(), alternatives.end(), [&](QPointF previous) {
                return std::hypot(previous.x() - offset.x(), previous.y() - offset.y()) < 1.0;
            }))
            alternatives.push_back(offset);
        if (alternatives.size() == 5)
            break;
    }
    return EditRetouchDonorSelection{
        .offset_radii = *best,
        .confidence = std::clamp(absolute_quality * (0.82 + 0.18 * separation), 0.0, 1.0),
        .candidate_count = candidate_count,
        .ranked_offsets = std::move(alternatives),
    };
}
