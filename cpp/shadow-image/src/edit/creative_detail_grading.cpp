#include "creative_detail_grading.hpp"

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

PreparedColorGradingWheel::PreparedColorGradingWheel(const double delta_a, const double delta_b,
                                                     const double delta_lightness) noexcept
    : delta_a_(delta_a), delta_b_(delta_b), delta_lightness_(delta_lightness) {}

PreparedColorGrading::PreparedColorGrading(PreparedColorGradingWheel shadows,
                                           PreparedColorGradingWheel midtones,
                                           PreparedColorGradingWheel highlights,
                                           const double center, const double width,
                                           const bool identity) noexcept
    : shadows_(std::move(shadows)), midtones_(std::move(midtones)),
      highlights_(std::move(highlights)), center_(center), width_(width), identity_(identity) {}

PreparedColorGrading prepare_color_grading(const SharpenAdjustment& parameters) noexcept {
    constexpr double pi = 3.141592653589793238462643383279502884;
    const auto prepare_wheel = [](const double hue, const double saturation,
                                  const double luminance) {
        const double angle = hue * pi / 180.0;
        return PreparedColorGradingWheel(0.09 * saturation * std::cos(angle),
                                         0.09 * saturation * std::sin(angle), 0.12 * luminance);
    };
    const bool identity =
        parameters.shadows_saturation == 0.0 && parameters.shadows_luminance == 0.0 &&
        parameters.midtones_saturation == 0.0 && parameters.midtones_luminance == 0.0 &&
        parameters.highlights_saturation == 0.0 && parameters.highlights_luminance == 0.0;
    return PreparedColorGrading(
        prepare_wheel(parameters.shadows_hue, parameters.shadows_saturation,
                      parameters.shadows_luminance),
        prepare_wheel(parameters.midtones_hue, parameters.midtones_saturation,
                      parameters.midtones_luminance),
        prepare_wheel(parameters.highlights_hue, parameters.highlights_saturation,
                      parameters.highlights_luminance),
        std::clamp(0.5 + 0.22 * parameters.grading_balance, 0.18, 0.82),
        0.08 + 0.30 * parameters.grading_blending, identity);
}

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr std::uint32_t minimum_rows_per_chunk = 32U;
constexpr double texture_sigma_level_zero = 1.4;
constexpr double clarity_small_sigma_level_zero = 2.4;
constexpr double clarity_large_sigma_level_zero = 12.0;

struct CreativeDetailPlan final {
    AdjustmentFootprint footprint;
    double texture_sigma_x = 0.0;
    double texture_sigma_y = 0.0;
    double clarity_small_sigma_x = 0.0;
    double clarity_small_sigma_y = 0.0;
    double clarity_large_sigma_x = 0.0;
    double clarity_large_sigma_y = 0.0;
    std::uint32_t local_contrast_radius = 1U;
    std::uint32_t local_contrast_small_radius = 1U;
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

template <typename Work> void parallel_for_creative_rows(const std::size_t height, Work&& work) {
    if (height > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("creative-detail height exceeds the row scheduler contract");
    }
    parallel_for_rows(static_cast<std::uint32_t>(height), minimum_rows_per_chunk,
                      std::forward<Work>(work));
}

[[nodiscard]] CreativeDetailPlan
prepare_creative_detail_plan(const SharpenAdjustment& parameters,
                             const double level_zero_to_raster_scale_x,
                             const double level_zero_to_raster_scale_y) {
    if (!std::isfinite(level_zero_to_raster_scale_x) || level_zero_to_raster_scale_x <= 0.0 ||
        !std::isfinite(level_zero_to_raster_scale_y) || level_zero_to_raster_scale_y <= 0.0) {
        throw EditError(EditErrorCode::invalid_parameter, std::nullopt,
                        "adjustment footprint scales must be finite and positive");
    }
    const auto normalized_amount = [](const double value) {
        return std::isfinite(value) && value >= -1.0 && value <= 1.0;
    };
    if (!normalized_amount(parameters.clarity) || !normalized_amount(parameters.texture) ||
        !normalized_amount(parameters.local_contrast) ||
        !std::isfinite(parameters.local_contrast_scale) || parameters.local_contrast_scale < 0.0 ||
        parameters.local_contrast_scale > 1.0) {
        throw EditError(EditErrorCode::invalid_parameter, std::nullopt,
                        "cannot calculate a footprint for malformed perceptual detail parameters");
    }

    CreativeDetailPlan plan;
    plan.texture_sigma_x = texture_sigma_level_zero * level_zero_to_raster_scale_x;
    plan.texture_sigma_y = texture_sigma_level_zero * level_zero_to_raster_scale_y;
    plan.clarity_small_sigma_x = clarity_small_sigma_level_zero * level_zero_to_raster_scale_x;
    plan.clarity_small_sigma_y = clarity_small_sigma_level_zero * level_zero_to_raster_scale_y;
    plan.clarity_large_sigma_x = clarity_large_sigma_level_zero * level_zero_to_raster_scale_x;
    plan.clarity_large_sigma_y = clarity_large_sigma_level_zero * level_zero_to_raster_scale_y;

    const double effective_raster_scale =
        std::sqrt(std::max(0.0, level_zero_to_raster_scale_x * level_zero_to_raster_scale_y));
    const double local_contrast_radius = std::max(
        1.0, std::ceil((20.0 + 60.0 * parameters.local_contrast_scale) * effective_raster_scale));
    if (local_contrast_radius > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw EditError(EditErrorCode::numeric_overflow, std::nullopt,
                        "creative-detail radius exceeds the supported integer range");
    }
    plan.local_contrast_radius = static_cast<std::uint32_t>(local_contrast_radius);
    plan.local_contrast_small_radius =
        std::max(1U, static_cast<std::uint32_t>(
                         std::ceil(static_cast<double>(plan.local_contrast_radius) * 0.32)));

    const bool neutral =
        parameters.clarity == 0.0 && parameters.texture == 0.0 && parameters.local_contrast == 0.0;
    if (neutral) {
        return plan;
    }
    const double local_support = 2.0 * (20.0 + 60.0 * parameters.local_contrast_scale);
    const double support_level_zero = std::max({
        parameters.texture == 0.0 ? 0.0 : 3.0 * texture_sigma_level_zero,
        parameters.clarity == 0.0 ? 0.0 : 3.0 * clarity_large_sigma_level_zero,
        parameters.local_contrast == 0.0 ? 0.0 : local_support,
    });
    const double horizontal = std::ceil(support_level_zero * level_zero_to_raster_scale_x);
    const double vertical = std::ceil(support_level_zero * level_zero_to_raster_scale_y);
    if (horizontal > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        vertical > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw EditError(EditErrorCode::numeric_overflow, std::nullopt,
                        "creative-detail footprint exceeds the supported integer range");
    }
    plan.footprint = AdjustmentFootprint{
        .horizontal_radius = static_cast<std::uint32_t>(horizontal),
        .vertical_radius = static_cast<std::uint32_t>(vertical),
    };
    return plan;
}

void apply_perceptual_detail(FloatRgbImage& image, const AdjustmentNode& node,
                             const std::size_t node_index, const SharpenAdjustment& parameters,
                             const CreativeDetailPlan& plan) {
    if (parameters.clarity == 0.0 && parameters.texture == 0.0 &&
        parameters.local_contrast == 0.0) {
        return;
    }
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw_node_error(EditErrorCode::numeric_overflow, node_index, node,
                         "perceptual detail working buffer exceeds the address space");
    }
    const std::size_t pixels = width * height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    std::vector<Vector3> oklab(pixels);
    std::vector<double> lightness(pixels);
    parallel_for_creative_rows(
        height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = y * stride;
                for (std::size_t x = 0U; x < width; ++x) {
                    const std::size_t pixel = y * width + x;
                    const std::size_t sample = row + x * rgb_channels;
                    oklab[pixel] = working_rgb_to_oklab(
                        color_transform, Vector3{
                                             static_cast<double>(image.samples[sample]),
                                             static_cast<double>(image.samples[sample + 1U]),
                                             static_cast<double>(image.samples[sample + 2U]),
                                         });
                    lightness[pixel] = oklab[pixel][0];
                }
            }
        });

    const double texture_sigma_x = plan.texture_sigma_x;
    const double texture_sigma_y = plan.texture_sigma_y;
    const double clarity_small_sigma_x = plan.clarity_small_sigma_x;
    const double clarity_small_sigma_y = plan.clarity_small_sigma_y;
    const double clarity_large_sigma_x = plan.clarity_large_sigma_x;
    const double clarity_large_sigma_y = plan.clarity_large_sigma_y;
    const std::uint32_t local_contrast_radius = plan.local_contrast_radius;
    const std::uint32_t local_contrast_small_radius = plan.local_contrast_small_radius;

    std::vector<double> texture_base;
    std::vector<double> clarity_small;
    std::vector<double> clarity_large;
    std::vector<float> local_contrast_small;
    std::vector<float> local_contrast_large;
    if (parameters.texture != 0.0) {
        texture_base =
            gaussian_blur_scalar(lightness, width, height, texture_sigma_x, texture_sigma_y);
    }
    if (parameters.clarity != 0.0) {
        clarity_small = gaussian_blur_scalar(lightness, width, height, clarity_small_sigma_x,
                                             clarity_small_sigma_y);
        clarity_large = gaussian_blur_scalar(lightness, width, height, clarity_large_sigma_x,
                                             clarity_large_sigma_y);
    }
    if (parameters.local_contrast != 0.0) {
        std::vector<float> guide(pixels);
        parallel_for_creative_rows(
            height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
                for (std::size_t y = first_row; y < past_last_row; ++y) {
                    const std::size_t row = y * width;
                    for (std::size_t x = 0U; x < width; ++x) {
                        guide[row + x] = static_cast<float>(lightness[row + x]);
                    }
                }
            });
        const PreparedGuidedFilter small_statistics =
            prepare_replicated_guided_filter(guide, width, height, local_contrast_small_radius);
        const PreparedGuidedFilter large_statistics =
            prepare_replicated_guided_filter(guide, width, height, local_contrast_radius);
        // Self-guidance preserves a strong luminance boundary instead of
        // averaging across it. This makes Local Contrast structurally
        // different from a broad unsharp mask and avoids its bright/dark halo.
        local_contrast_small = apply_guided_self_filter(guide, small_statistics, 8.0e-4);
        local_contrast_large = apply_guided_self_filter(guide, large_statistics, 1.6e-3);
    }

    const auto compress_detail = [](const double value, const double knee) noexcept {
        return value / (1.0 + std::abs(value) / knee);
    };
    parallel_for_creative_rows(height, [&](const std::uint32_t first_row,
                                           const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = y * stride;
            for (std::size_t x = 0U; x < width; ++x) {
                const std::size_t pixel = y * width + x;
                Vector3 output_lab = oklab[pixel];
                // Avoid exposing unstable residuals in the near-black toe, while
                // allowing a negative value to soften detail as naturally as a
                // positive value enhances it.
                const double shadow_protection = smooth_transition(0.015, 0.090, output_lab[0]);
                if (parameters.texture != 0.0) {
                    const double residual = output_lab[0] - texture_base[pixel];
                    output_lab[0] += parameters.texture * 0.70 * compress_detail(residual, 0.035) *
                                     shadow_protection;
                }
                if (parameters.clarity != 0.0) {
                    const double high_frequency = output_lab[0] - clarity_small[pixel];
                    const double mid_frequency = clarity_small[pixel] - clarity_large[pixel];
                    // A broad Gaussian cannot follow a hard edge. Fade that band
                    // there so the operator improves local structure rather than
                    // producing a dark/light outline around high-contrast edges.
                    const double edge_protection =
                        1.0 - smooth_transition(0.018, 0.085, std::abs(high_frequency));
                    output_lab[0] += parameters.clarity * 1.15 *
                                     compress_detail(mid_frequency, 0.090) * edge_protection *
                                     shadow_protection;
                }
                if (parameters.local_contrast != 0.0) {
                    const double broad_residual = static_cast<double>(local_contrast_small[pixel]) -
                                                  static_cast<double>(local_contrast_large[pixel]);
                    const double edge_residual =
                        output_lab[0] - static_cast<double>(local_contrast_small[pixel]);
                    // The guided separation already respects an edge; this second
                    // guard gracefully fades the remaining response at a very hard
                    // boundary, which is where even an edge-aware local operator
                    // otherwise risks looking like a halo at 100% inspection.
                    const double edge_protection =
                        1.0 - smooth_transition(0.030, 0.120, std::abs(edge_residual));
                    output_lab[0] += parameters.local_contrast * 1.20 *
                                     compress_detail(broad_residual, 0.115) * edge_protection *
                                     shadow_protection;
                }
                const Vector3 output = oklab_to_working_rgb(color_transform, output_lab);
                const std::size_t sample = row + x * rgb_channels;
                image.samples[sample] = checked_edit_pixel_float(output[0], node_index, node);
                image.samples[sample + 1U] = checked_edit_pixel_float(output[1], node_index, node);
                image.samples[sample + 2U] = checked_edit_pixel_float(output[2], node_index, node);
            }
        }
    });
}

void apply_color_grading(FloatRgbImage& image, const AdjustmentNode& node,
                         const std::size_t node_index, const PreparedColorGrading& prepared) {
    if (prepared.is_identity()) {
        return;
    }
    const auto luma_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    transform_rgb_pixels(
        image, node_index, node, [&prepared, luma_weights, &color_transform](const Vector3& input) {
            const double luma = input[0] * luma_weights[0] + input[1] * luma_weights[1] +
                                input[2] * luma_weights[2];
            Vector3 lab = working_rgb_to_oklab(color_transform, input);
            const double normalized = std::max(0.0, luma) / (std::max(0.0, luma) + 0.18);
            double shadow_weight =
                1.0 - smooth_transition(prepared.center() - prepared.width(),
                                        prepared.center() + prepared.width(), normalized);
            double highlight_weight =
                smooth_transition(prepared.center() - prepared.width(),
                                  prepared.center() + prepared.width(), normalized);
            double midtone_weight = 1.0 - std::abs(normalized - prepared.center()) /
                                              std::max(0.12, 0.5 + prepared.width());
            midtone_weight = std::clamp(midtone_weight, 0.0, 1.0);
            const double total = shadow_weight + midtone_weight + highlight_weight;
            shadow_weight /= total;
            midtone_weight /= total;
            highlight_weight /= total;
            const auto apply_wheel = [&lab](const PreparedColorGradingWheel& wheel,
                                            const double weight) {
                lab[0] += wheel.delta_lightness() * weight;
                lab[1] += wheel.delta_a() * weight;
                lab[2] += wheel.delta_b() * weight;
            };
            apply_wheel(prepared.shadows(), shadow_weight);
            apply_wheel(prepared.midtones(), midtone_weight);
            apply_wheel(prepared.highlights(), highlight_weight);
            return oklab_to_working_rgb(color_transform, lab);
        });
}

} // namespace

AdjustmentFootprint creative_detail_footprint(const SharpenAdjustment& parameters,
                                              const double level_zero_to_raster_scale_x,
                                              const double level_zero_to_raster_scale_y) {
    return prepare_creative_detail_plan(parameters, level_zero_to_raster_scale_x,
                                        level_zero_to_raster_scale_y)
        .footprint;
}

void apply_creative_detail_grading_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                       const std::size_t node_index,
                                       const SharpenAdjustment& parameters) {
    const CreativeDetailPlan plan = prepare_creative_detail_plan(
        parameters, image.level_zero_to_raster_scale_x, image.level_zero_to_raster_scale_y);
    const PreparedColorGrading grading = prepare_color_grading(parameters);
    apply_perceptual_detail(image, node, node_index, parameters, plan);
    apply_color_grading(image, node, node_index, grading);
}

} // namespace shadow::image::detail
