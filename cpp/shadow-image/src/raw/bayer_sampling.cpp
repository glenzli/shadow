#include "bayer_sampling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace shadow::image::detail {

namespace {

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

[[nodiscard]] std::size_t cfa_site(
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    return static_cast<std::size_t>((raw_y & 1U) * 2U + (raw_x & 1U));
}

[[nodiscard]] RawCfaColor cfa_color_at(
    const RawFrameDescriptor& descriptor,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    return descriptor.bayer_2x2[cfa_site(raw_x, raw_y)];
}

[[nodiscard]] float normalized_sample(
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
    return static_cast<float>(
        (static_cast<double>(frame.samples[index]) - black) / (white - black)
    );
}

[[nodiscard]] float sensor_clip_evidence(const float normalized) noexcept {
    // Preserve a small shoulder below the declared white level: some camera encoders reserve
    // one or two codes below that level, but the ratio is already not trustworthy there.
    return std::clamp((normalized - 0.98F) * 50.0F, 0.0F, 1.0F);
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
            std::string(operation)
                + " requires at least a two-by-two stored Bayer sensor plane"
        );
    }
}

CameraRgbSample bilinear_camera_rgb_sample_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) {
    const auto& descriptor = frame.descriptor;
    const auto width = descriptor.storage_dimensions.width;
    const auto height = descriptor.storage_dimensions.height;
    std::array<double, 3U> totals{};
    std::array<double, 3U> clipped_totals{};
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
            const auto normalized = normalized_sample(frame, x, y);
            totals[index] += normalized;
            clipped_totals[index] += sensor_clip_evidence(normalized);
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
        result.values[channel] = static_cast<float>(
            totals[channel] / static_cast<double>(counts[channel])
        );
        result.sensor_clip_coverage[channel] = static_cast<float>(
            clipped_totals[channel] / static_cast<double>(counts[channel])
        );
    }
    return result;
}

CameraRgb bilinear_camera_rgb_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) {
    return bilinear_camera_rgb_sample_at(frame, raw_x, raw_y).values;
}

BayerAreaSamplingGrid make_bayer_area_sampling_grid(
    const RawFrame& frame,
    const Dimensions target_dimensions
) {
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
    const std::uint32_t target_y
) {
    const auto& descriptor = frame.descriptor;
    const double source_top = static_cast<double>(descriptor.active_margins.top)
        + static_cast<double>(target_y) * grid.scale_y;
    const double source_bottom = static_cast<double>(descriptor.active_margins.top)
        + static_cast<double>(target_y + 1U) * grid.scale_y;
    const double source_left = static_cast<double>(descriptor.active_margins.left)
        + static_cast<double>(target_x) * grid.scale_x;
    const double source_right = static_cast<double>(descriptor.active_margins.left)
        + static_cast<double>(target_x + 1U) * grid.scale_x;
    const auto first_source_y = static_cast<std::uint32_t>(std::floor(source_top));
    const auto last_source_y = static_cast<std::uint32_t>(std::ceil(source_bottom));
    const auto first_source_x = static_cast<std::uint32_t>(std::floor(source_left));
    const auto last_source_x = static_cast<std::uint32_t>(std::ceil(source_right));

    std::array<double, 3U> totals{};
    std::array<double, 3U> weights{};
    std::array<double, 3U> clipped_weights{};
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
            const auto normalized = normalized_sample(frame, raw_x, raw_y);
            totals[index] += normalized * weight;
            weights[index] += weight;
            clipped_weights[index] += sensor_clip_evidence(normalized) * weight;
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
            return bilinear_camera_rgb_sample_at(frame, center_x, center_y);
        }
        result.values[channel] = static_cast<float>(totals[channel] / weights[channel]);
        result.sensor_clip_coverage[channel] = static_cast<float>(
            clipped_weights[channel] / weights[channel]
        );
    }
    return result;
}

CameraRgb area_camera_rgb_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    const std::uint32_t target_x,
    const std::uint32_t target_y
) {
    return area_camera_rgb_sample_at(frame, grid, target_x, target_y).values;
}

} // namespace shadow::image::detail
