#include <shadow/image/edit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace shadow::image {

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr std::size_t maximum_spot_heal_targets = 64U;
constexpr std::uint16_t minimum_spot_radius_level_zero = 1U;
constexpr std::uint16_t maximum_spot_radius_level_zero = 128U;

[[noreturn]] void invalid_retouch(const std::string_view detail) {
    throw EditError(
        EditErrorCode::invalid_parameter,
        std::nullopt,
        "spot-heal adjustment " + std::string(detail)
    );
}

[[nodiscard]] Dimensions execution_full_dimensions(
    const FloatRgbImage& image,
    const AdjustmentExecutionContext context
) {
    const Dimensions full = context.full_dimensions.width == 0U
            || context.full_dimensions.height == 0U
        ? image.dimensions
        : context.full_dimensions;
    if (full.width == 0U || full.height == 0U
        || context.origin_x > full.width || context.origin_y > full.height
        || image.dimensions.width > full.width - context.origin_x
        || image.dimensions.height > full.height - context.origin_y) {
        invalid_retouch("execution context exceeds full-image dimensions");
    }
    return full;
}

[[nodiscard]] double smoothstep(const double edge0, const double edge1, const double value) {
    if (edge1 <= edge0) {
        return value < edge1 ? 0.0 : 1.0;
    }
    const double normalized = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

[[nodiscard]] std::size_t sample_index(
    const FloatRgbImage& image,
    const std::uint32_t x,
    const std::uint32_t y
) {
    return static_cast<std::size_t>(y) * (image.row_stride_bytes / sizeof(float))
        + static_cast<std::size_t>(x) * rgb_channels;
}

[[nodiscard]] std::array<double, rgb_channels> sample_bilinear(
    const FloatRgbImage& image,
    const double x,
    const double y
) {
    const double clamped_x = std::clamp(
        x,
        0.0,
        static_cast<double>(image.dimensions.width - 1U)
    );
    const double clamped_y = std::clamp(
        y,
        0.0,
        static_cast<double>(image.dimensions.height - 1U)
    );
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const auto x1 = std::min(x0 + 1U, image.dimensions.width - 1U);
    const auto y1 = std::min(y0 + 1U, image.dimensions.height - 1U);
    const double blend_x = clamped_x - static_cast<double>(x0);
    const double blend_y = clamped_y - static_cast<double>(y0);
    const std::size_t top_left = sample_index(image, x0, y0);
    const std::size_t top_right = sample_index(image, x1, y0);
    const std::size_t bottom_left = sample_index(image, x0, y1);
    const std::size_t bottom_right = sample_index(image, x1, y1);

    std::array<double, rgb_channels> result{};
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        const double top = std::lerp(
            static_cast<double>(image.samples[top_left + channel]),
            static_cast<double>(image.samples[top_right + channel]),
            blend_x
        );
        const double bottom = std::lerp(
            static_cast<double>(image.samples[bottom_left + channel]),
            static_cast<double>(image.samples[bottom_right + channel]),
            blend_x
        );
        result[channel] = std::lerp(top, bottom, blend_y);
    }
    return result;
}

void apply_target(
    FloatRgbImage& image,
    const SpotHealTarget target,
    const AdjustmentExecutionContext context,
    const Dimensions full
) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U) {
        invalid_retouch("requires a non-empty raster");
    }
    const double radius_x = static_cast<double>(target.radius_level_zero_pixels)
        * image.level_zero_to_raster_scale_x;
    const double radius_y = static_cast<double>(target.radius_level_zero_pixels)
        * image.level_zero_to_raster_scale_y;
    if (!std::isfinite(radius_x) || !std::isfinite(radius_y)
        || radius_x <= 0.0 || radius_y <= 0.0) {
        invalid_retouch("has an invalid proxy scale");
    }

    // Pixel-centre coordinates map exactly to the normalized original image
    // contract used by local masks. The input tile may contain a detail apron.
    const double center_x = target.center_x * static_cast<double>(full.width)
        - static_cast<double>(context.origin_x) - 0.5;
    const double center_y = target.center_y * static_cast<double>(full.height)
        - static_cast<double>(context.origin_y) - 0.5;
    const double outer_x = radius_x * 2.0;
    const double outer_y = radius_y * 2.0;
    const auto lower_x = static_cast<std::int64_t>(std::floor(center_x - outer_x));
    const auto upper_x = static_cast<std::int64_t>(std::ceil(center_x + outer_x));
    const auto lower_y = static_cast<std::int64_t>(std::floor(center_y - outer_y));
    const auto upper_y = static_cast<std::int64_t>(std::ceil(center_y + outer_y));
    const auto clamp_x = [&image](const std::int64_t value) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            value,
            0,
            static_cast<std::int64_t>(image.dimensions.width) - 1
        ));
    };
    const auto clamp_y = [&image](const std::int64_t value) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            value,
            0,
            static_cast<std::int64_t>(image.dimensions.height) - 1
        ));
    };
    const FloatRgbImage source = image;
    std::array<double, rgb_channels> repair{};
    if (target.mode == SpotRepairMode::heal) {
        std::array<double, rgb_channels> ring_sum{};
        std::uint64_t ring_count = 0U;
        constexpr double ring_min = 1.18;
        constexpr double ring_max = 1.92;
        for (std::int64_t y = lower_y; y <= upper_y; ++y) {
            for (std::int64_t x = lower_x; x <= upper_x; ++x) {
                const double dx = (static_cast<double>(x) - center_x) / radius_x;
                const double dy = (static_cast<double>(y) - center_y) / radius_y;
                const double distance = std::sqrt(std::fma(dx, dx, dy * dy));
                if (distance < ring_min || distance > ring_max) {
                    continue;
                }
                const std::size_t sample = sample_index(source, clamp_x(x), clamp_y(y));
                for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                    ring_sum[channel] += source.samples[sample + channel];
                }
                ++ring_count;
            }
        }
        if (ring_count == 0U) {
            return;
        }
        for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
            repair[channel] = ring_sum[channel] / static_cast<double>(ring_count);
            if (!std::isfinite(repair[channel])) {
                invalid_retouch("surrounding-pixel reconstruction overflowed");
            }
        }
    }

    const auto paint_lower_x = static_cast<std::int64_t>(std::floor(center_x - radius_x));
    const auto paint_upper_x = static_cast<std::int64_t>(std::ceil(center_x + radius_x));
    const auto paint_lower_y = static_cast<std::int64_t>(std::floor(center_y - radius_y));
    const auto paint_upper_y = static_cast<std::int64_t>(std::ceil(center_y + radius_y));
    const std::int64_t raster_max_x = static_cast<std::int64_t>(image.dimensions.width) - 1;
    const std::int64_t raster_max_y = static_cast<std::int64_t>(image.dimensions.height) - 1;
    for (std::int64_t y = std::max<std::int64_t>(0, paint_lower_y);
         y <= std::min(raster_max_y, paint_upper_y);
         ++y) {
        for (std::int64_t x = std::max<std::int64_t>(0, paint_lower_x);
             x <= std::min(raster_max_x, paint_upper_x);
             ++x) {
            const double dx = (static_cast<double>(x) - center_x) / radius_x;
            const double dy = (static_cast<double>(y) - center_y) / radius_y;
            const double distance = std::sqrt(std::fma(dx, dx, dy * dy));
            if (distance > 1.0) {
                continue;
            }
            const double feather_start = 1.0 - target.feather;
            const double alpha = target.feather <= 0.0
                ? 1.0
                : 1.0 - smoothstep(feather_start, 1.0, distance);
            const std::size_t sample = sample_index(
                image,
                static_cast<std::uint32_t>(x),
                static_cast<std::uint32_t>(y)
            );
            const std::array<double, rgb_channels> replacement =
                target.mode == SpotRepairMode::clone
                ? sample_bilinear(
                    source,
                    static_cast<double>(x) + target.source_offset_x_radii * radius_x,
                    static_cast<double>(y) + target.source_offset_y_radii * radius_y
                )
                : repair;
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const double value = std::fma(
                    alpha,
                    replacement[channel]
                        - static_cast<double>(source.samples[sample + channel]),
                    static_cast<double>(source.samples[sample + channel])
                );
                if (!std::isfinite(value)
                    || value < static_cast<double>(std::numeric_limits<float>::lowest())
                    || value > static_cast<double>(std::numeric_limits<float>::max())) {
                    invalid_retouch("produced an invalid RGB sample");
                }
                image.samples[sample + channel] = static_cast<float>(value);
            }
        }
    }
}

} // namespace

void validate_spot_heal(const SpotHealAdjustment& adjustment) {
    if (adjustment.spots.empty() || adjustment.spots.size() > maximum_spot_heal_targets) {
        invalid_retouch("must contain between 1 and 64 targets");
    }
    for (const auto& target : adjustment.spots) {
        if (!std::isfinite(target.center_x) || !std::isfinite(target.center_y)
            || target.center_x < 0.0 || target.center_x > 1.0
            || target.center_y < 0.0 || target.center_y > 1.0
            || target.radius_level_zero_pixels < minimum_spot_radius_level_zero
            || target.radius_level_zero_pixels > maximum_spot_radius_level_zero
            || (target.mode != SpotRepairMode::heal
                && target.mode != SpotRepairMode::clone)
            || !std::isfinite(target.source_offset_x_radii)
            || !std::isfinite(target.source_offset_y_radii)
            || target.source_offset_x_radii < -2.0
            || target.source_offset_x_radii > 2.0
            || target.source_offset_y_radii < -2.0
            || target.source_offset_y_radii > 2.0
            || !std::isfinite(target.feather)
            || target.feather < 0.0 || target.feather > 1.0) {
            invalid_retouch("target coordinates or radius are outside the supported range");
        }
    }
}

void apply_spot_heal(
    FloatRgbImage& image,
    const SpotHealAdjustment& adjustment,
    const AdjustmentExecutionContext context
) {
    validate_spot_heal(adjustment);
    const Dimensions full = execution_full_dimensions(image, context);
    for (const auto& target : adjustment.spots) {
        apply_target(image, target, context, full);
    }
}

} // namespace shadow::image
