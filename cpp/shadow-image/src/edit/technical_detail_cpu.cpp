#include "technical_detail_cpu.hpp"

#include "adjustment_node_diagnostics.hpp"
#include "rgb_pixel_traversal.hpp"
#include "scalar_neighborhood_filters.hpp"
#include "working_color_math.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::uint32_t minimum_rows_per_chunk = 32U;

struct TechnicalDetailPlan final {
    AdjustmentFootprint footprint;
    std::uint32_t sharpen_kernel_radius_x = 0U;
    std::uint32_t sharpen_kernel_radius_y = 0U;
    double sharpen_sigma_x = 0.0;
    double sharpen_sigma_y = 0.0;
    std::uint32_t denoise_fine_radius = 1U;
    std::uint32_t denoise_coarse_radius = 2U;
    double denoise_fine_epsilon = 0.0003;
    double denoise_coarse_epsilon = 0.0007;
    double denoise_coarse_mix = 0.10;
    double bilateral_inverse_range = 0.0;
    double bilateral_sigma_x = 1.0;
    double bilateral_sigma_y = 1.0;
    std::int64_t bilateral_radius_x = 1;
    std::int64_t bilateral_radius_y = 1;
};

[[nodiscard]] double smooth_transition(const double lower, const double upper,
                                       const double value) noexcept {
    if (value <= lower) {
        return 0.0;
    }
    if (value >= upper) {
        return 1.0;
    }
    const double normalized = (value - lower) / (upper - lower);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

[[nodiscard]] double circular_hue_degrees(double value) noexcept {
    value = std::fmod(value, 360.0);
    return value < 0.0 ? value + 360.0 : value;
}

template <typename Work> void parallel_for_technical_rows(const std::size_t height, Work&& work) {
    if (height > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("technical-detail height exceeds the row scheduler contract");
    }
    parallel_for_rows(static_cast<std::uint32_t>(height), minimum_rows_per_chunk,
                      std::forward<Work>(work));
}

[[nodiscard]] TechnicalDetailPlan
prepare_technical_detail_plan(const SharpenAdjustment& parameters,
                              const double level_zero_to_raster_scale_x,
                              const double level_zero_to_raster_scale_y) {
    if (!std::isfinite(level_zero_to_raster_scale_x) || level_zero_to_raster_scale_x <= 0.0 ||
        !std::isfinite(level_zero_to_raster_scale_y) || level_zero_to_raster_scale_y <= 0.0) {
        throw EditError(EditErrorCode::invalid_parameter, std::nullopt,
                        "adjustment footprint scales must be finite and positive");
    }
    if (!std::isfinite(parameters.amount) || parameters.amount < 0.0 || parameters.amount > 2.0 ||
        !std::isfinite(parameters.radius) || parameters.radius < 0.1 || parameters.radius > 5.0 ||
        !std::isfinite(parameters.threshold) || parameters.threshold < 0.0 ||
        parameters.threshold > 1.0 || !std::isfinite(parameters.masking) ||
        parameters.masking < 0.0 || parameters.masking > 1.0) {
        throw EditError(EditErrorCode::invalid_parameter, std::nullopt,
                        "cannot calculate a footprint for malformed sharpen parameters");
    }

    TechnicalDetailPlan plan;
    const bool denoise_active =
        parameters.denoise_luminance > 0.0 || parameters.denoise_color > 0.0;
    const double denoise_strength =
        std::max(parameters.denoise_luminance, parameters.denoise_color);
    const double high_strength_response = denoise_strength * denoise_strength;
    const double denoise_authority =
        high_strength_response * (1.0 - 0.60 * parameters.denoise_detail);
    plan.denoise_fine_radius =
        static_cast<std::uint32_t>(std::clamp(std::ceil(1.0 + 2.0 * denoise_authority), 1.0, 3.0));
    plan.denoise_coarse_radius =
        static_cast<std::uint32_t>(std::clamp(std::ceil(2.0 + 5.0 * denoise_authority), 2.0, 7.0));
    plan.denoise_fine_epsilon = 0.0003 + 0.006 * denoise_authority;
    plan.denoise_coarse_epsilon = 0.0007 + 0.015 * denoise_authority;
    plan.denoise_coarse_mix = std::clamp(0.10 + 0.38 * denoise_authority, 0.0, 0.50);

    const double sharpen_horizontal =
        parameters.amount == 0.0
            ? 0.0
            : std::ceil(3.0 * parameters.radius * level_zero_to_raster_scale_x);
    const double sharpen_vertical =
        parameters.amount == 0.0
            ? 0.0
            : std::ceil(3.0 * parameters.radius * level_zero_to_raster_scale_y);
    const double denoise_support =
        denoise_active ? static_cast<double>(plan.denoise_coarse_radius) : 0.0;
    const double horizontal = sharpen_horizontal + denoise_support;
    const double vertical = sharpen_vertical + denoise_support;
    if (horizontal > std::numeric_limits<std::uint32_t>::max() ||
        vertical > std::numeric_limits<std::uint32_t>::max()) {
        throw EditError(EditErrorCode::numeric_overflow, std::nullopt,
                        "adjustment footprint exceeds the supported integer range");
    }
    plan.footprint = AdjustmentFootprint{
        .horizontal_radius = static_cast<std::uint32_t>(horizontal),
        .vertical_radius = static_cast<std::uint32_t>(vertical),
    };
    plan.sharpen_kernel_radius_x = static_cast<std::uint32_t>(sharpen_horizontal);
    plan.sharpen_kernel_radius_y = static_cast<std::uint32_t>(sharpen_vertical);
    plan.sharpen_sigma_x = parameters.radius * level_zero_to_raster_scale_x;
    plan.sharpen_sigma_y = parameters.radius * level_zero_to_raster_scale_y;

    const double range_sigma =
        0.025 + 0.18 * (1.0 - parameters.denoise_detail) +
        0.16 * high_strength_response * (1.0 - 0.75 * parameters.denoise_detail);
    plan.bilateral_inverse_range = 1.0 / (2.0 * range_sigma * range_sigma);
    const double denoise_native_sigma = 1.5 + 0.75 * high_strength_response;
    const double minimum_proxy_sigma = 0.65 + 0.35 * high_strength_response;
    plan.bilateral_sigma_x =
        std::max(minimum_proxy_sigma, denoise_native_sigma * level_zero_to_raster_scale_x);
    plan.bilateral_sigma_y =
        std::max(minimum_proxy_sigma, denoise_native_sigma * level_zero_to_raster_scale_y);
    constexpr double denoise_native_support = 2.0;
    const double bilateral_radius_x =
        std::ceil(denoise_native_support * level_zero_to_raster_scale_x);
    const double bilateral_radius_y =
        std::ceil(denoise_native_support * level_zero_to_raster_scale_y);
    if (denoise_active &&
        (bilateral_radius_x > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
         bilateral_radius_y > static_cast<double>(std::numeric_limits<std::int64_t>::max()))) {
        throw EditError(EditErrorCode::numeric_overflow, std::nullopt,
                        "bilateral denoise radius exceeds the supported integer range");
    }
    if (denoise_active) {
        plan.bilateral_radius_x =
            std::max<std::int64_t>(1, static_cast<std::int64_t>(bilateral_radius_x));
        plan.bilateral_radius_y =
            std::max<std::int64_t>(1, static_cast<std::int64_t>(bilateral_radius_y));
    }
    return plan;
}

void apply_sharpen(FloatRgbImage& image, const AdjustmentNode& node, const std::size_t node_index,
                   const SharpenAdjustment& parameters, const TechnicalDetailPlan& plan) {
    if (parameters.amount == 0.0) {
        return;
    }

    const auto kernel_x = gaussian_kernel(plan.sharpen_sigma_x, plan.sharpen_kernel_radius_x);
    const auto kernel_y = gaussian_kernel(plan.sharpen_sigma_y, plan.sharpen_kernel_radius_y);

    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw_node_error(EditErrorCode::numeric_overflow, node_index, node,
                         "sharpen working buffer exceeds the address space");
    }
    const std::size_t pixels = width * height;
    std::vector<double> log_luminance(pixels);
    std::vector<double> horizontal_blur(pixels);
    std::vector<double> blurred(pixels);
    const auto weights = image.working_space.luminance_coefficients;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    constexpr double minimum_positive_luminance = 5.9604644775390625e-8; // 2^-24

    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t row = y * stride;
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t sample = row + x * rgb_channels;
            const double luminance = static_cast<double>(image.samples[sample]) * weights[0] +
                                     static_cast<double>(image.samples[sample + 1U]) * weights[1] +
                                     static_cast<double>(image.samples[sample + 2U]) * weights[2];
            log_luminance[y * width + x] =
                std::log2(std::max(luminance, minimum_positive_luminance));
        }
    }

    const auto radius_x = static_cast<std::int64_t>(plan.sharpen_kernel_radius_x);
    const auto radius_y = static_cast<std::int64_t>(plan.sharpen_kernel_radius_y);
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -radius_x; offset <= radius_x; ++offset) {
                const std::size_t source_x =
                    reflect101_index(static_cast<std::int64_t>(x) + offset, width);
                sum += log_luminance[y * width + source_x] *
                       kernel_x[static_cast<std::size_t>(offset + radius_x)];
            }
            horizontal_blur[y * width + x] = sum;
        }
    }
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -radius_y; offset <= radius_y; ++offset) {
                const std::size_t source_y =
                    reflect101_index(static_cast<std::int64_t>(y) + offset, height);
                sum += horizontal_blur[source_y * width + x] *
                       kernel_y[static_cast<std::size_t>(offset + radius_y)];
            }
            blurred[y * width + x] = sum;
        }
    }

    const double threshold_ev = parameters.threshold * 0.25;
    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t row = y * stride;
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t pixel = y * width + x;
            const std::size_t sample = row + x * rgb_channels;
            const double luminance = static_cast<double>(image.samples[sample]) * weights[0] +
                                     static_cast<double>(image.samples[sample + 1U]) * weights[1] +
                                     static_cast<double>(image.samples[sample + 2U]) * weights[2];
            if (luminance <= minimum_positive_luminance) {
                continue;
            }

            const double detail = log_luminance[pixel] - blurred[pixel];
            // The tiny guard makes a mathematically flat field exactly neutral despite the
            // unavoidable roundoff of a normalized separable convolution.
            if (std::abs(detail) <= 1.0e-12) {
                continue;
            }
            const double thresholded =
                std::copysign(std::max(0.0, std::abs(detail) - threshold_ev), detail);
            if (thresholded == 0.0) {
                continue;
            }
            const double edge_confidence =
                smooth_transition(threshold_ev, threshold_ev + 0.25, std::abs(detail));
            const double mask = (1.0 - parameters.masking) + parameters.masking * edge_confidence;
            const double gain = std::exp2(parameters.amount * thresholded * mask);
            image.samples[sample] = checked_edit_pixel_float(
                static_cast<double>(image.samples[sample]) * gain, node_index, node);
            image.samples[sample + 1U] = checked_edit_pixel_float(
                static_cast<double>(image.samples[sample + 1U]) * gain, node_index, node);
            image.samples[sample + 2U] = checked_edit_pixel_float(
                static_cast<double>(image.samples[sample + 2U]) * gain, node_index, node);
        }
    }
}

void apply_multiscale_guided_denoise(FloatRgbImage& image, const AdjustmentNode& node,
                                     const std::size_t node_index,
                                     const SharpenAdjustment& parameters,
                                     const TechnicalDetailPlan& plan) {
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    if (width == 0U || height == 0U || width > std::numeric_limits<std::size_t>::max() / height) {
        throw_node_error(EditErrorCode::numeric_overflow, node_index, node,
                         "guided denoise working buffer exceeds the address space");
    }
    const std::size_t pixels = width * height;
    const auto weights = image.working_space.luminance_coefficients;
    if (weights[1] <= 0.0) {
        throw_node_error(
            EditErrorCode::invalid_parameter, node_index, node,
            "guided denoise requires a working space with a positive green luminance weight");
    }
    std::vector<float> luma(pixels);
    std::vector<float> red_chroma(pixels);
    std::vector<float> blue_chroma(pixels);
    parallel_for_technical_rows(
        height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t source_row = y * stride;
                const std::size_t target_row = y * width;
                for (std::size_t x = 0U; x < width; ++x) {
                    const std::size_t sample = source_row + x * rgb_channels;
                    const double lightness =
                        static_cast<double>(image.samples[sample]) * weights[0] +
                        static_cast<double>(image.samples[sample + 1U]) * weights[1] +
                        static_cast<double>(image.samples[sample + 2U]) * weights[2];
                    const std::size_t pixel = target_row + x;
                    luma[pixel] = static_cast<float>(lightness);
                    red_chroma[pixel] = static_cast<float>(image.samples[sample] - lightness);
                    blue_chroma[pixel] = static_cast<float>(image.samples[sample + 2U] - lightness);
                }
            }
        });

    const std::uint32_t fine_radius = plan.denoise_fine_radius;
    const std::uint32_t coarse_radius = plan.denoise_coarse_radius;
    const double fine_epsilon = plan.denoise_fine_epsilon;
    const double coarse_epsilon = plan.denoise_coarse_epsilon;
    const double coarse_mix = plan.denoise_coarse_mix;

    std::vector<float> fine_luma;
    std::vector<float> fine_red_chroma;
    std::vector<float> fine_blue_chroma;
    {
        const auto guide = prepare_replicated_guided_filter(luma, width, height, fine_radius);
        if (parameters.denoise_luminance > 0.0) {
            fine_luma = apply_guided_self_filter(luma, guide, fine_epsilon);
        }
        if (parameters.denoise_color > 0.0) {
            fine_red_chroma = apply_guided_target_filter(luma, guide, red_chroma, fine_epsilon);
            fine_blue_chroma = apply_guided_target_filter(luma, guide, blue_chroma, fine_epsilon);
        }
    }
    std::vector<float> coarse_luma;
    std::vector<float> coarse_red_chroma;
    std::vector<float> coarse_blue_chroma;
    {
        const auto guide = prepare_replicated_guided_filter(luma, width, height, coarse_radius);
        if (parameters.denoise_luminance > 0.0) {
            coarse_luma = apply_guided_self_filter(luma, guide, coarse_epsilon);
        }
        if (parameters.denoise_color > 0.0) {
            coarse_red_chroma = apply_guided_target_filter(luma, guide, red_chroma, coarse_epsilon);
            coarse_blue_chroma =
                apply_guided_target_filter(luma, guide, blue_chroma, coarse_epsilon);
        }
    }

    parallel_for_technical_rows(height, [&](const std::uint32_t first_row,
                                            const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t source_row = y * stride;
            const std::size_t target_row = y * width;
            for (std::size_t x = 0U; x < width; ++x) {
                const std::size_t pixel = target_row + x;
                const std::size_t sample = source_row + x * rgb_channels;
                const double filtered_luma =
                    parameters.denoise_luminance == 0.0
                        ? luma[pixel]
                        : std::lerp(static_cast<double>(fine_luma[pixel]),
                                    static_cast<double>(coarse_luma[pixel]), coarse_mix);
                const double filtered_red_chroma =
                    parameters.denoise_color == 0.0
                        ? red_chroma[pixel]
                        : std::lerp(static_cast<double>(fine_red_chroma[pixel]),
                                    static_cast<double>(coarse_red_chroma[pixel]), coarse_mix);
                const double filtered_blue_chroma =
                    parameters.denoise_color == 0.0
                        ? blue_chroma[pixel]
                        : std::lerp(static_cast<double>(fine_blue_chroma[pixel]),
                                    static_cast<double>(coarse_blue_chroma[pixel]), coarse_mix);
                const double output_luma = std::lerp(static_cast<double>(luma[pixel]),
                                                     filtered_luma, parameters.denoise_luminance);
                const double output_red_chroma =
                    std::lerp(static_cast<double>(red_chroma[pixel]), filtered_red_chroma,
                              parameters.denoise_color);
                const double output_blue_chroma =
                    std::lerp(static_cast<double>(blue_chroma[pixel]), filtered_blue_chroma,
                              parameters.denoise_color);
                image.samples[sample] =
                    checked_edit_pixel_float(output_luma + output_red_chroma, node_index, node);
                image.samples[sample + 1U] =
                    checked_edit_pixel_float(output_luma - (weights[0] * output_red_chroma +
                                                            weights[2] * output_blue_chroma) /
                                                               weights[1],
                                             node_index, node);
                image.samples[sample + 2U] =
                    checked_edit_pixel_float(output_luma + output_blue_chroma, node_index, node);
            }
        }
    });
}

void apply_bilateral_fallback_denoise(FloatRgbImage& image, const AdjustmentNode& node,
                                      const std::size_t node_index,
                                      const SharpenAdjustment& parameters,
                                      const TechnicalDetailPlan& plan) {
    if (parameters.denoise_luminance == 0.0 && parameters.denoise_color == 0.0) {
        return;
    }
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const auto source = image.samples;
    const auto weights = image.working_space.luminance_coefficients;
    const double inverse_range = plan.bilateral_inverse_range;
    const double sigma_x = plan.bilateral_sigma_x;
    const double sigma_y = plan.bilateral_sigma_y;
    const std::int64_t radius_x = plan.bilateral_radius_x;
    const std::int64_t radius_y = plan.bilateral_radius_y;
    const double inverse_two_sigma_x_squared = 1.0 / (2.0 * sigma_x * sigma_x);
    const double inverse_two_sigma_y_squared = 1.0 / (2.0 * sigma_y * sigma_y);
    parallel_for_technical_rows(height, [&](const std::uint32_t first_row,
                                            const std::uint32_t past_last_row) {
        for (std::uint32_t y = first_row; y < past_last_row; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const std::size_t center = static_cast<std::size_t>(y) * stride + x * rgb_channels;
                const Vector3 original{
                    source[center],
                    source[center + 1U],
                    source[center + 2U],
                };
                const double original_luma =
                    original[0] * weights[0] + original[1] * weights[1] + original[2] * weights[2];
                Vector3 filtered{};
                double weight_sum = 0.0;
                for (std::int64_t dy = -radius_y; dy <= radius_y; ++dy) {
                    const std::size_t source_y =
                        reflect101_index(static_cast<std::int64_t>(y) + dy, height);
                    for (std::int64_t dx = -radius_x; dx <= radius_x; ++dx) {
                        const std::size_t source_x =
                            reflect101_index(static_cast<std::int64_t>(x) + dx, width);
                        const std::size_t sample = source_y * stride + source_x * rgb_channels;
                        const Vector3 neighbor{
                            source[sample],
                            source[sample + 1U],
                            source[sample + 2U],
                        };
                        const double neighbor_luma = neighbor[0] * weights[0] +
                                                     neighbor[1] * weights[1] +
                                                     neighbor[2] * weights[2];
                        const double delta = neighbor_luma - original_luma;
                        const double spatial =
                            static_cast<double>(dx * dx) * inverse_two_sigma_x_squared +
                            static_cast<double>(dy * dy) * inverse_two_sigma_y_squared;
                        const double weight = std::exp(-spatial - delta * delta * inverse_range);
                        for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                            filtered[channel] += neighbor[channel] * weight;
                        }
                        weight_sum += weight;
                    }
                }
                for (double& channel : filtered) {
                    channel /= weight_sum;
                }
                const double filtered_luma =
                    filtered[0] * weights[0] + filtered[1] * weights[1] + filtered[2] * weights[2];
                const double luminance =
                    std::lerp(original_luma, filtered_luma, parameters.denoise_luminance);
                for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                    const double original_chroma = original[channel] - original_luma;
                    const double filtered_chroma = filtered[channel] - filtered_luma;
                    const double output = luminance + std::lerp(original_chroma, filtered_chroma,
                                                                parameters.denoise_color);
                    image.samples[center + channel] =
                        checked_edit_pixel_float(output, node_index, node);
                }
            }
        }
    });
}

void apply_edge_aware_denoise(FloatRgbImage& image, const AdjustmentNode& node,
                              const std::size_t node_index, const SharpenAdjustment& parameters,
                              const TechnicalDetailPlan& plan) {
    if (parameters.denoise_luminance == 0.0 && parameters.denoise_color == 0.0) {
        return;
    }
    // A normal proxy or full-detail tile has enough samples for the two-scale guided path.
    // Keep the previous tiny bilateral implementation only as a well-defined fallback for
    // degenerate images used by defensive callers and small contract fixtures.
    if (image.dimensions.width < 3U || image.dimensions.height < 3U) {
        apply_bilateral_fallback_denoise(image, node, node_index, parameters, plan);
        return;
    }
    apply_multiscale_guided_denoise(image, node, node_index, parameters, plan);
}

void apply_dehaze_and_defringe(FloatRgbImage& image, const AdjustmentNode& node,
                               const std::size_t node_index, const SharpenAdjustment& parameters) {
    if (parameters.dehaze == 0.0 && parameters.defringe_purple_amount == 0.0 &&
        parameters.defringe_green_amount == 0.0) {
        return;
    }
    const auto luma_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    transform_rgb_pixels(
        image, node_index, node, [&parameters, luma_weights, &color_transform](Vector3 input) {
            const double luma = input[0] * luma_weights[0] + input[1] * luma_weights[1] +
                                input[2] * luma_weights[2];
            if (parameters.dehaze > 0.0) {
                const double veil = std::max(0.0, std::min({input[0], input[1], input[2]}));
                const double maximum = std::max({0.0, input[0], input[1], input[2]});
                const double veil_fraction = std::clamp(veil / (maximum + 0.18), 0.0, 1.0);
                const double transmission =
                    std::max(0.2, 1.0 - 0.88 * parameters.dehaze * veil_fraction);
                for (double& channel : input) {
                    channel = (channel - parameters.dehaze * 0.65 * veil) / transmission;
                }
            } else if (parameters.dehaze < 0.0) {
                const double amount = -parameters.dehaze;
                const double atmosphere = std::max(0.18, luma + 0.28);
                for (double& channel : input) {
                    channel = std::lerp(channel, atmosphere, 0.55 * amount);
                }
            }

            Vector3 lab = working_rgb_to_oklab(color_transform, input);
            const double chroma = std::hypot(lab[1], lab[2]);
            if ((parameters.defringe_purple_amount > 0.0 ||
                 parameters.defringe_green_amount > 0.0) &&
                chroma > 1.0e-8) {
                const double hue = circular_hue_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
                constexpr double range_feather_degrees = 10.0;
                const auto range_weight = [hue](const double low, const double high) {
                    return smooth_transition(low - range_feather_degrees, low, hue) *
                           (1.0 - smooth_transition(high, high + range_feather_degrees, hue));
                };
                const double purple = parameters.defringe_purple_amount *
                                      range_weight(parameters.defringe_purple_hue_low,
                                                   parameters.defringe_purple_hue_high);
                const double green = parameters.defringe_green_amount *
                                     range_weight(parameters.defringe_green_hue_low,
                                                  parameters.defringe_green_hue_high);
                const double reduction = std::max(purple, green);
                lab[1] *= 1.0 - 0.9 * reduction;
                lab[2] *= 1.0 - 0.9 * reduction;
            }

            return oklab_to_working_rgb(color_transform, lab);
        });
}

} // namespace

AdjustmentFootprint technical_detail_footprint(const SharpenAdjustment& parameters,
                                               const double level_zero_to_raster_scale_x,
                                               const double level_zero_to_raster_scale_y) {
    return prepare_technical_detail_plan(parameters, level_zero_to_raster_scale_x,
                                         level_zero_to_raster_scale_y)
        .footprint;
}

void apply_technical_detail_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                const std::size_t node_index, const SharpenAdjustment& parameters) {
    const TechnicalDetailPlan plan = prepare_technical_detail_plan(
        parameters, image.level_zero_to_raster_scale_x, image.level_zero_to_raster_scale_y);
    apply_edge_aware_denoise(image, node, node_index, parameters, plan);
    apply_dehaze_and_defringe(image, node, node_index, parameters);
    apply_sharpen(image, node, node_index, parameters, plan);
}

} // namespace shadow::image::detail
