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
    const int ix = std::clamp(static_cast<int>(std::lround(x)), 0, width - 1);
    const int iy = std::clamp(static_cast<int>(std::lround(y)), 0, height - 1);
    const std::size_t index = static_cast<std::size_t>(iy) * request.preview_row_stride_bytes
                              + static_cast<std::size_t>(ix) * RGB_CHANNELS;
    constexpr double inverse_byte = 1.0 / 255.0;
    return {
        .red = static_cast<double>(request.preview_rgb8[index]) * inverse_byte,
        .green = static_cast<double>(request.preview_rgb8[index + 1U]) * inverse_byte,
        .blue = static_cast<double>(request.preview_rgb8[index + 2U]) * inverse_byte,
    };
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

[[nodiscard]] double
gradient_energy(const EditRetouchDonorRequest& request, const double x, const double y) noexcept {
    const double horizontal =
        luma(sample_rgb8(request, x + 1.0, y)) - luma(sample_rgb8(request, x - 1.0, y));
    const double vertical =
        luma(sample_rgb8(request, x, y + 1.0)) - luma(sample_rgb8(request, x, y - 1.0));
    return std::fma(horizontal, horizontal, vertical * vertical);
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
    const double color_weight = request.mode == EditRetouchDonorMode::Clone ? 1.0 : 0.28;
    double score = 0.0;
    std::size_t sample_count = 0U;
    for (const QPointF point : path) {
        for (const double angle : angles) {
            const double ring_x = std::cos(angle) * radius_x * 1.12;
            const double ring_y = std::sin(angle) * radius_y * 1.12;
            const double target_x = point.x() + ring_x;
            const double target_y = point.y() + ring_y;
            const double source_x = target_x + offset_x;
            const double source_y = target_y + offset_y;
            const Sample target = sample_rgb8(request, target_x, target_y);
            const Sample source = sample_rgb8(request, source_x, source_y);
            score += color_weight * color_distance(target, source);
            const double target_gradient = gradient_energy(request, target_x, target_y);
            const double source_gradient = gradient_energy(request, source_x, source_y);
            const double gradient_delta = std::sqrt(target_gradient) - std::sqrt(source_gradient);
            score += 1.8 * gradient_delta * gradient_delta;
            ++sample_count;
        }
    }
    return sample_count == 0U ? std::numeric_limits<double>::infinity()
                              : score / static_cast<double>(sample_count);
}

} // namespace

std::optional<QPointF> select_edit_retouch_donor_offset(const EditRetouchDonorRequest& request) {
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

    constexpr std::array<double, 6U> distances{2.5, 3.25, 4.0, 5.0, 6.25, 7.5};
    constexpr std::size_t direction_count = 16U;
    double best_score = std::numeric_limits<double>::infinity();
    std::optional<QPointF> best;
    for (const double distance : distances) {
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
            const double score =
                candidate_score(request, path, offset_x, offset_y, radius_x, radius_y);
            if (score < best_score) {
                best_score = score;
                best = QPointF(offset_x_radii, offset_y_radii);
            }
        }
    }
    return best;
}
