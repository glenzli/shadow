#include "bayer_sampling.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace shadow::image::detail {

namespace {

constexpr float highlight_shoulder_start = 0.88F;

[[nodiscard]] float cfa_highlight_risk(const float normalized_sensor) noexcept {
    const float t = std::clamp(
        (normalized_sensor - highlight_shoulder_start) / (1.0F - highlight_shoulder_start),
        0.0F,
        1.0F
    );
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] int rgb_channel(const RawCfaColor color) noexcept {
    switch (color) {
    case RawCfaColor::red:
        return 0;
    case RawCfaColor::green:
        return 1;
    case RawCfaColor::blue:
        return 2;
    case RawCfaColor::unknown:
        return -1;
    }
    return -1;
}

[[nodiscard]] std::size_t cfa_site(const std::uint32_t raw_x, const std::uint32_t raw_y) noexcept {
    return static_cast<std::size_t>((raw_y & 1U) * 2U + (raw_x & 1U));
}

[[nodiscard]] RawCfaColor cfa_color_at(
    const RawFrameDescriptor& descriptor,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    return descriptor.bayer_2x2[cfa_site(raw_x, raw_y)];
}

[[nodiscard]] float normalized_sensor_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto site = cfa_site(raw_x, raw_y);
    const auto width = static_cast<std::size_t>(descriptor.storage_dimensions.width);
    const auto index = static_cast<std::size_t>(raw_y) * width + raw_x;
    const double black = descriptor.black_levels[site];
    const double white = descriptor.white_levels[site];
    const double normalized = (static_cast<double>(frame.samples[index]) - black) / (white - black);
    return static_cast<float>(normalized);
}

[[nodiscard]] float normalized_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform
) noexcept {
    const auto site = cfa_site(raw_x, raw_y);
    double normalized = normalized_sensor_sample(frame, raw_x, raw_y);
    if (transform != nullptr && transform->apply_cfa_white_balance) {
        normalized *= transform->cfa_white_balance[site];
    }
    return static_cast<float>(normalized);
}

[[nodiscard]] RawCfaFootprint cfa_highlight_risk_footprint_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    const auto& dimensions = frame.descriptor.storage_dimensions;
    const auto max_even_x = (dimensions.width - 2U) & ~1U;
    const auto max_even_y = (dimensions.height - 2U) & ~1U;
    const auto base_x = std::min(raw_x & ~1U, max_even_x);
    const auto base_y = std::min(raw_y & ~1U, max_even_y);
    RawCfaFootprint result{};
    for (std::uint32_t dy = 0U; dy < 2U; ++dy) {
        for (std::uint32_t dx = 0U; dx < 2U; ++dx) {
            const auto x = base_x + dx;
            const auto y = base_y + dy;
            result[cfa_site(x, y)] = cfa_highlight_risk(normalized_sensor_sample(frame, x, y));
        }
    }
    return result;
}

[[nodiscard]] bool in_sensor_bounds(
    const RawFrameDescriptor& descriptor,
    const std::int64_t raw_x,
    const std::int64_t raw_y
) noexcept {
    return raw_x >= 0 && raw_y >= 0
           && raw_x < static_cast<std::int64_t>(descriptor.storage_dimensions.width)
           && raw_y < static_cast<std::int64_t>(descriptor.storage_dimensions.height);
}

[[nodiscard]] std::optional<float> directional_green_estimate(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto center_colour = cfa_color_at(descriptor, raw_x, raw_y);
    if (center_colour == RawCfaColor::green) {
        return normalized_sample(frame, raw_x, raw_y, transform);
    }
    if (center_colour != RawCfaColor::red && center_colour != RawCfaColor::blue) {
        return std::nullopt;
    }

    const auto try_direction =
        [&](const std::int64_t dx,
            const std::int64_t dy) -> std::optional<std::pair<float, float>> {
        const auto left_x = static_cast<std::int64_t>(raw_x) - dx;
        const auto left_y = static_cast<std::int64_t>(raw_y) - dy;
        const auto right_x = static_cast<std::int64_t>(raw_x) + dx;
        const auto right_y = static_cast<std::int64_t>(raw_y) + dy;
        const auto far_left_x = static_cast<std::int64_t>(raw_x) - 2 * dx;
        const auto far_left_y = static_cast<std::int64_t>(raw_y) - 2 * dy;
        const auto far_right_x = static_cast<std::int64_t>(raw_x) + 2 * dx;
        const auto far_right_y = static_cast<std::int64_t>(raw_y) + 2 * dy;
        if (!in_sensor_bounds(descriptor, left_x, left_y)
            || !in_sensor_bounds(descriptor, right_x, right_y)
            || !in_sensor_bounds(descriptor, far_left_x, far_left_y)
            || !in_sensor_bounds(descriptor, far_right_x, far_right_y)) {
            return std::nullopt;
        }
        const auto as_u32 = [](const std::int64_t coordinate) noexcept {
            return static_cast<std::uint32_t>(coordinate);
        };
        if (cfa_color_at(descriptor, as_u32(left_x), as_u32(left_y)) != RawCfaColor::green
            || cfa_color_at(descriptor, as_u32(right_x), as_u32(right_y)) != RawCfaColor::green
            || cfa_color_at(descriptor, as_u32(far_left_x), as_u32(far_left_y)) != center_colour
            || cfa_color_at(descriptor, as_u32(far_right_x), as_u32(far_right_y))
                   != center_colour) {
            return std::nullopt;
        }
        const float left = normalized_sample(frame, as_u32(left_x), as_u32(left_y), transform);
        const float right = normalized_sample(frame, as_u32(right_x), as_u32(right_y), transform);
        const float far_left =
            normalized_sample(frame, as_u32(far_left_x), as_u32(far_left_y), transform);
        const float far_right =
            normalized_sample(frame, as_u32(far_right_x), as_u32(far_right_y), transform);
        const float center = normalized_sample(frame, raw_x, raw_y, transform);
        const float chroma_laplacian = 2.0F * center - far_left - far_right;
        const float estimate = 0.5F * (left + right) + 0.25F * chroma_laplacian;
        const float gradient = std::abs(left - right) + std::abs(chroma_laplacian);
        return std::pair<float, float>{estimate, gradient};
    };

    const auto horizontal = try_direction(1, 0);
    const auto vertical = try_direction(0, 1);
    if (horizontal.has_value() && vertical.has_value()) {
        constexpr float epsilon = 1.0e-5F;
        const float horizontal_weight = 1.0F / (epsilon + horizontal->second);
        const float vertical_weight = 1.0F / (epsilon + vertical->second);
        return (horizontal->first * horizontal_weight + vertical->first * vertical_weight)
               / (horizontal_weight + vertical_weight);
    }
    if (horizontal.has_value()) {
        return horizontal->first;
    }
    if (vertical.has_value()) {
        return vertical->first;
    }
    return std::nullopt;
}

} // namespace

void validate_bayer_frame(const RawFrame& frame, const char* operation) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            std::string(operation) + " requires a valid owned RAW frame"
        );
    }
    if (!frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            std::string(operation) + " requires an explicit Bayer two-by-two CFA layout"
        );
    }
    if (frame.descriptor.storage_dimensions.width < 2U
        || frame.descriptor.storage_dimensions.height < 2U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            std::string(operation) + " requires at least a two-by-two stored Bayer sensor plane"
        );
    }
}

CameraRgbSample bilinear_camera_rgb_sample_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform
) {
    const auto& descriptor = frame.descriptor;
    const auto width = descriptor.storage_dimensions.width;
    const auto height = descriptor.storage_dimensions.height;
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    for (int dy = -1; dy <= 1; ++dy) {
        const auto candidate_y = static_cast<std::int64_t>(raw_y) + dy;
        if (candidate_y < 0 || candidate_y >= static_cast<std::int64_t>(height)) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            const auto candidate_x = static_cast<std::int64_t>(raw_x) + dx;
            if (candidate_x < 0 || candidate_x >= static_cast<std::int64_t>(width)) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(candidate_x);
            const auto y = static_cast<std::uint32_t>(candidate_y);
            const int channel = rgb_channel(cfa_color_at(descriptor, x, y));
            if (channel < 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(channel);
            const auto normalized = normalized_sample(frame, x, y, transform);
            totals[index] += normalized;
            ++counts[index];
        }
    }

    CameraRgbSample result;
    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        if (counts[channel] == 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "Bayer reconstruction found no same-colour neighbour"
            );
        }
        result.values[channel] =
            static_cast<float>(totals[channel] / static_cast<double>(counts[channel]));
    }
    result.cfa_highlight_risk = cfa_highlight_risk_footprint_at(frame, raw_x, raw_y);
    return result;
}

CameraRgb bilinear_camera_rgb_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform
) {
    return bilinear_camera_rgb_sample_at(frame, raw_x, raw_y, transform).values;
}

CameraRgbSample edge_aware_camera_rgb_sample_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform
) {
    const CameraRgbSample bilinear = bilinear_camera_rgb_sample_at(frame, raw_x, raw_y, transform);
    const auto& descriptor = frame.descriptor;
    const auto center_colour = cfa_color_at(descriptor, raw_x, raw_y);
    const int center_channel = rgb_channel(center_colour);
    const auto green = directional_green_estimate(frame, raw_x, raw_y, transform);
    if (center_channel < 0 || !green.has_value()) {
        return bilinear;
    }

    CameraRgbSample result = bilinear;
    result.values[1U] = *green;
    const auto reconstruct_colour_difference = [&](const RawCfaColor target_colour,
                                                   const std::size_t target_channel) {
        if (center_colour == target_colour) {
            result.values[target_channel] = normalized_sample(frame, raw_x, raw_y, transform);
            return;
        }
        double weighted_sum = 0.0;
        double total_weight = 0.0;
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            const auto candidate_y = static_cast<std::int64_t>(raw_y) + dy;
            if (candidate_y < 0
                || candidate_y >= static_cast<std::int64_t>(descriptor.storage_dimensions.height)) {
                continue;
            }
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const auto candidate_x = static_cast<std::int64_t>(raw_x) + dx;
                if (candidate_x < 0
                    || candidate_x
                           >= static_cast<std::int64_t>(descriptor.storage_dimensions.width)) {
                    continue;
                }
                const auto x = static_cast<std::uint32_t>(candidate_x);
                const auto y = static_cast<std::uint32_t>(candidate_y);
                if (cfa_color_at(descriptor, x, y) != target_colour) {
                    continue;
                }
                const auto neighbour_green = directional_green_estimate(frame, x, y, transform);
                if (!neighbour_green.has_value()) {
                    continue;
                }
                const double weight = dx == 0 || dy == 0 ? 1.0 : 0.7071067811865476;
                weighted_sum +=
                    weight
                    * (static_cast<double>(normalized_sample(frame, x, y, transform))
                       + static_cast<double>(*green) - static_cast<double>(*neighbour_green));
                total_weight += weight;
            }
        }
        if (total_weight > 0.0) {
            result.values[target_channel] = static_cast<float>(weighted_sum / total_weight);
        }
    };
    reconstruct_colour_difference(RawCfaColor::red, 0U);
    reconstruct_colour_difference(RawCfaColor::blue, 2U);
    return result;
}

BayerAreaSamplingGrid
make_bayer_area_sampling_grid(const RawFrame& frame, const Dimensions target_dimensions) {
    if (target_dimensions.width == 0U || target_dimensions.height == 0U
        || target_dimensions.width > frame.descriptor.active_dimensions.width
        || target_dimensions.height > frame.descriptor.active_dimensions.height) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer area sampling target is outside the active sensor dimensions"
        );
    }
    return BayerAreaSamplingGrid{
        .target_dimensions = target_dimensions,
        .scale_x = static_cast<double>(frame.descriptor.active_dimensions.width)
                   / static_cast<double>(target_dimensions.width),
        .scale_y = static_cast<double>(frame.descriptor.active_dimensions.height)
                   / static_cast<double>(target_dimensions.height),
    };
}

CameraRgbSample area_camera_rgb_sample_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    const std::uint32_t target_x,
    const std::uint32_t target_y,
    const RawFrameLinearTransform* const transform
) {
    const auto& descriptor = frame.descriptor;
    // This is also reached by the isolated decode helper.  A malformed provider frame or a
    // rounded last footprint must therefore become a normal DecodeError, never an unchecked
    // read past the owned sensor plane in a worker thread.
    if (grid.target_dimensions.width == 0U || grid.target_dimensions.height == 0U
        || target_x >= grid.target_dimensions.width || target_y >= grid.target_dimensions.height
        || !std::isfinite(grid.scale_x) || !std::isfinite(grid.scale_y) || grid.scale_x <= 0.0
        || grid.scale_y <= 0.0) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer area sampling received an invalid target coordinate or sampling grid"
        );
    }

    const double active_left = static_cast<double>(descriptor.active_margins.left);
    const double active_top = static_cast<double>(descriptor.active_margins.top);
    const double active_right =
        active_left + static_cast<double>(descriptor.active_dimensions.width);
    const double active_bottom =
        active_top + static_cast<double>(descriptor.active_dimensions.height);
    const auto active_right_exclusive = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(descriptor.active_margins.left)
        + descriptor.active_dimensions.width
    );
    const auto active_bottom_exclusive = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(descriptor.active_margins.top)
        + descriptor.active_dimensions.height
    );
    const double unclamped_source_top = active_top + static_cast<double>(target_y) * grid.scale_y;
    const double unclamped_source_bottom =
        active_top + static_cast<double>(target_y + 1U) * grid.scale_y;
    const double unclamped_source_left = active_left + static_cast<double>(target_x) * grid.scale_x;
    const double unclamped_source_right =
        active_left + static_cast<double>(target_x + 1U) * grid.scale_x;
    const double source_top = std::clamp(unclamped_source_top, active_top, active_bottom);
    const double source_bottom = std::clamp(unclamped_source_bottom, active_top, active_bottom);
    const double source_left = std::clamp(unclamped_source_left, active_left, active_right);
    const double source_right = std::clamp(unclamped_source_right, active_left, active_right);
    if (source_left >= source_right || source_top >= source_bottom) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer area sampling footprint is outside the active sensor rectangle"
        );
    }
    // Clamp after ceil as well.  Exact rational scale factors can round one ulp above the active
    // edge, and the previous implementation then dereferenced one sample beyond the final row or
    // column.  The active rectangle is validated to sit inside storage by RawFrame::valid().
    const auto first_source_y = static_cast<std::uint32_t>(std::floor(source_top));
    const auto last_source_y =
        std::min(active_bottom_exclusive, static_cast<std::uint32_t>(std::ceil(source_bottom)));
    const auto first_source_x = static_cast<std::uint32_t>(std::floor(source_left));
    const auto last_source_x =
        std::min(active_right_exclusive, static_cast<std::uint32_t>(std::ceil(source_right)));

    std::array<double, 3U> totals{};
    std::array<double, 3U> weights{};
    std::array<float, 4U> risk_maximums{};
    std::array<double, 4U> risk_weights{};
    for (std::uint32_t raw_y = first_source_y; raw_y < last_source_y; ++raw_y) {
        const double overlap_y = std::max(
            0.0,
            std::min(source_bottom, static_cast<double>(raw_y + 1U))
                - std::max(source_top, static_cast<double>(raw_y))
        );
        for (std::uint32_t raw_x = first_source_x; raw_x < last_source_x; ++raw_x) {
            const double overlap_x = std::max(
                0.0,
                std::min(source_right, static_cast<double>(raw_x + 1U))
                    - std::max(source_left, static_cast<double>(raw_x))
            );
            const int channel = rgb_channel(cfa_color_at(descriptor, raw_x, raw_y));
            if (channel < 0) {
                continue;
            }
            const double weight = overlap_x * overlap_y;
            const auto index = static_cast<std::size_t>(channel);
            const auto normalized = normalized_sample(frame, raw_x, raw_y, transform);
            totals[index] += normalized * weight;
            weights[index] += weight;
            const auto site = cfa_site(raw_x, raw_y);
            risk_maximums[site] = std::max(
                risk_maximums[site],
                cfa_highlight_risk(normalized_sensor_sample(frame, raw_x, raw_y))
            );
            risk_weights[site] += weight;
        }
    }

    CameraRgbSample result;
    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        if (weights[channel] <= 0.0) {
            const auto center_x = std::min(
                descriptor.storage_dimensions.width - 1U,
                static_cast<std::uint32_t>((source_left + source_right) * 0.5)
            );
            const auto center_y = std::min(
                descriptor.storage_dimensions.height - 1U,
                static_cast<std::uint32_t>((source_top + source_bottom) * 0.5)
            );
            return bilinear_camera_rgb_sample_at(frame, center_x, center_y, transform);
        }
        result.values[channel] = static_cast<float>(totals[channel] / weights[channel]);
    }
    for (std::size_t site = 0U; site < result.cfa_highlight_risk.size(); ++site) {
        if (risk_weights[site] <= 0.0) {
            const auto center_x = std::min(
                descriptor.storage_dimensions.width - 1U,
                static_cast<std::uint32_t>((source_left + source_right) * 0.5)
            );
            const auto center_y = std::min(
                descriptor.storage_dimensions.height - 1U,
                static_cast<std::uint32_t>((source_top + source_bottom) * 0.5)
            );
            return bilinear_camera_rgb_sample_at(frame, center_x, center_y, transform);
        }
        result.cfa_highlight_risk[site] = risk_maximums[site];
    }
    return result;
}

CameraRgb area_camera_rgb_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    const std::uint32_t target_x,
    const std::uint32_t target_y,
    const RawFrameLinearTransform* const transform
) {
    return area_camera_rgb_sample_at(frame, grid, target_x, target_y, transform).values;
}

} // namespace shadow::image::detail
