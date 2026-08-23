#include "clipped_highlight_reconstruction.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace shadow::image::raw_pipeline_detail {

namespace {

constexpr std::uint32_t maximum_guide_edge = 384U;
constexpr float minimum_luminance = 1.0e-6F;
// A projected output pixel is factually clipped when any contributing CFA sample reaches sensor
// white, but that is not evidence that the whole RGB surface is missing. Require spatially
// coherent clipping before replacing it with the low-frequency surface; this tapers isolated
// Bayer-phase hits instead of growing edge hairs.
constexpr float surface_reconstruction_support_begin = 0.035F;
constexpr float surface_reconstruction_support_full = 0.20F;
constexpr std::array<float, 3U> luminance_weights{0.2126F, 0.7152F, 0.0722F};

struct GuideLevel final {
    Dimensions dimensions;
    std::vector<float> log_luminance;
    std::vector<float> observed_log_luminance;
    std::vector<float> observed_luminance_confidence;
    std::vector<float> observed_chroma_r;
    std::vector<float> observed_chroma_g;
    std::vector<float> observed_chroma_confidence;
    std::vector<float> chroma_r;
    std::vector<float> chroma_g;
    std::vector<float> luminance_confidence;
    std::vector<float> chroma_confidence;
};

[[nodiscard]] std::size_t checked_count(const Dimensions dimensions) {
    const auto count = dimensions.pixel_count();
    if (count == 0U
        || count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "clipped-highlight reconstruction dimensions exceed addressable memory"
        );
    }
    return static_cast<std::size_t>(count);
}

[[nodiscard]] Dimensions guide_dimensions(const Dimensions source) noexcept {
    const auto longest = std::max(source.width, source.height);
    if (longest <= maximum_guide_edge) {
        return source;
    }
    const auto scaled = [longest](const std::uint32_t extent) {
        return std::max<std::uint32_t>(
            1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(extent) * maximum_guide_edge + longest / 2U) / longest
            )
        );
    };
    return {scaled(source.width), scaled(source.height)};
}

[[nodiscard]] std::uint32_t bin_begin(
    const std::uint32_t coordinate,
    const std::uint32_t source_extent,
    const std::uint32_t target_extent
) noexcept {
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(coordinate) * source_extent / target_extent
    );
}

[[nodiscard]] std::uint32_t bin_end(
    const std::uint32_t coordinate,
    const std::uint32_t source_extent,
    const std::uint32_t target_extent
) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(coordinate + 1U) * source_extent + target_extent - 1U)
        / target_extent
    );
}

[[nodiscard]] float smoothstep(const float low, const float high, const float value) noexcept {
    const float normalized = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return normalized * normalized * (3.0F - 2.0F * normalized);
}

[[nodiscard]] float positive_luminance(const float r, const float g, const float b) noexcept {
    return std::max(
        minimum_luminance,
        std::max(0.0F, r) * luminance_weights[0U] + std::max(0.0F, g) * luminance_weights[1U]
            + std::max(0.0F, b) * luminance_weights[2U]
    );
}

[[nodiscard]] GuideLevel prepare_finest_guide(
    const SceneLinearRgbFrame& source,
    const SensorClippingMask& clipping,
    const HighlightChromaRiskMap& highlight_chroma_risk,
    const Dimensions dimensions
) {
    GuideLevel guide{
        .dimensions = dimensions,
        .log_luminance = std::vector<float>(checked_count(dimensions)),
        .observed_log_luminance = std::vector<float>(checked_count(dimensions)),
        .observed_luminance_confidence = std::vector<float>(checked_count(dimensions)),
        .observed_chroma_r = std::vector<float>(checked_count(dimensions)),
        .observed_chroma_g = std::vector<float>(checked_count(dimensions)),
        .observed_chroma_confidence = std::vector<float>(checked_count(dimensions)),
        .chroma_r = std::vector<float>(checked_count(dimensions)),
        .chroma_g = std::vector<float>(checked_count(dimensions)),
        .luminance_confidence = std::vector<float>(checked_count(dimensions)),
        .chroma_confidence = std::vector<float>(checked_count(dimensions)),
    };
    for (std::uint32_t guide_y = 0U; guide_y < dimensions.height; ++guide_y) {
        const auto source_y_begin = bin_begin(guide_y, source.dimensions.height, dimensions.height);
        const auto source_y_end = bin_end(guide_y, source.dimensions.height, dimensions.height);
        for (std::uint32_t guide_x = 0U; guide_x < dimensions.width; ++guide_x) {
            const auto source_x_begin =
                bin_begin(guide_x, source.dimensions.width, dimensions.width);
            const auto source_x_end = bin_end(guide_x, source.dimensions.width, dimensions.width);
            double log_luminance_sum = 0.0;
            double observed_log_luminance_sum = 0.0;
            double observed_luminance_weight_sum = 0.0;
            double observed_chroma_r_sum = 0.0;
            double observed_chroma_g_sum = 0.0;
            double observed_chroma_weight_sum = 0.0;
            double chroma_r_sum = 0.0;
            double chroma_g_sum = 0.0;
            double luminance_weight_sum = 0.0;
            double chroma_weight_sum = 0.0;
            std::uint64_t total_count = 0U;
            for (std::uint32_t source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                for (std::uint32_t source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                    const auto pixel =
                        static_cast<std::size_t>(source_y) * source.dimensions.width + source_x;
                    ++total_count;
                    const auto sample = pixel * 3U;
                    const float red = std::max(0.0F, source.samples[sample]);
                    const float green = std::max(0.0F, source.samples[sample + 1U]);
                    const float blue = std::max(0.0F, source.samples[sample + 2U]);
                    const float luminance = positive_luminance(red, green, blue);
                    const float luminance_weight = smoothstep(0.18F, 0.75F, luminance);
                    observed_log_luminance_sum +=
                        static_cast<double>(std::log2(luminance)) * luminance_weight;
                    observed_luminance_weight_sum += luminance_weight;
                    const float observed_sum = std::max(red + green + blue, minimum_luminance);
                    const float observed_chroma_weight = smoothstep(0.48F, 0.90F, luminance);
                    observed_chroma_r_sum +=
                        static_cast<double>(red / observed_sum) * observed_chroma_weight;
                    observed_chroma_g_sum +=
                        static_cast<double>(green / observed_sum) * observed_chroma_weight;
                    observed_chroma_weight_sum += observed_chroma_weight;
                    if ((clipping.samples[pixel] & sensor_highlight_clipped) != 0U) {
                        continue;
                    }
                    log_luminance_sum +=
                        static_cast<double>(std::log2(luminance)) * luminance_weight;
                    luminance_weight_sum += luminance_weight;
                    const float chroma_confidence =
                        1.0F
                        - smoothstep(
                            0.08F,
                            0.70F,
                            static_cast<float>(highlight_chroma_risk.samples[pixel]) / 255.0F
                        );
                    const float chroma_weight = luminance_weight * chroma_confidence;
                    if (chroma_weight <= 0.0F) {
                        continue;
                    }
                    const float sum = std::max(red + green + blue, minimum_luminance);
                    // A physical highlight may border a dark silhouette. That silhouette is
                    // trustworthy image content but not a plausible continuation of the lost
                    // highlight surface, so admit measured guide samples continuously by their
                    // scene-linear highlight luminance instead of letting a black edge pull the
                    // entire reconstruction down.
                    chroma_r_sum += static_cast<double>(red / sum) * chroma_weight;
                    chroma_g_sum += static_cast<double>(green / sum) * chroma_weight;
                    chroma_weight_sum += chroma_weight;
                }
            }
            const auto index = static_cast<std::size_t>(guide_y) * dimensions.width + guide_x;
            if (observed_luminance_weight_sum > 0.0) {
                guide.observed_log_luminance[index] =
                    static_cast<float>(observed_log_luminance_sum / observed_luminance_weight_sum);
                guide.observed_luminance_confidence[index] = std::min(
                    1.0F,
                    static_cast<float>(
                        observed_luminance_weight_sum / static_cast<double>(total_count)
                    )
                );
                if (observed_chroma_weight_sum > 0.0) {
                    guide.observed_chroma_r[index] =
                        static_cast<float>(observed_chroma_r_sum / observed_chroma_weight_sum);
                    guide.observed_chroma_g[index] =
                        static_cast<float>(observed_chroma_g_sum / observed_chroma_weight_sum);
                    guide.observed_chroma_confidence[index] = std::min(
                        1.0F,
                        static_cast<float>(
                            observed_chroma_weight_sum / static_cast<double>(total_count)
                        )
                    );
                }
            }
            if (luminance_weight_sum > 0.0) {
                guide.log_luminance[index] =
                    static_cast<float>(log_luminance_sum / luminance_weight_sum);
                guide.luminance_confidence[index] = std::min(
                    1.0F,
                    static_cast<float>(luminance_weight_sum / static_cast<double>(total_count))
                );
            }
            if (chroma_weight_sum > 0.0) {
                guide.chroma_r[index] = static_cast<float>(chroma_r_sum / chroma_weight_sum);
                guide.chroma_g[index] = static_cast<float>(chroma_g_sum / chroma_weight_sum);
                guide.chroma_confidence[index] = std::min(
                    1.0F,
                    static_cast<float>(chroma_weight_sum / static_cast<double>(total_count))
                );
            }
        }
    }
    return guide;
}

[[nodiscard]] GuideLevel reduce_guide(const GuideLevel& fine) {
    const Dimensions dimensions{
        std::max(1U, (fine.dimensions.width + 1U) / 2U),
        std::max(1U, (fine.dimensions.height + 1U) / 2U),
    };
    GuideLevel coarse{
        .dimensions = dimensions,
        .log_luminance = std::vector<float>(checked_count(dimensions)),
        .observed_log_luminance = {},
        .observed_luminance_confidence = {},
        .observed_chroma_r = {},
        .observed_chroma_g = {},
        .observed_chroma_confidence = {},
        .chroma_r = std::vector<float>(checked_count(dimensions)),
        .chroma_g = std::vector<float>(checked_count(dimensions)),
        .luminance_confidence = std::vector<float>(checked_count(dimensions)),
        .chroma_confidence = std::vector<float>(checked_count(dimensions)),
    };
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            double luminance_weight_sum = 0.0;
            double chroma_weight_sum = 0.0;
            double log_luminance_sum = 0.0;
            double chroma_r_sum = 0.0;
            double chroma_g_sum = 0.0;
            for (std::uint32_t child_y = y * 2U;
                 child_y < std::min(fine.dimensions.height, y * 2U + 2U);
                 ++child_y) {
                for (std::uint32_t child_x = x * 2U;
                     child_x < std::min(fine.dimensions.width, x * 2U + 2U);
                     ++child_x) {
                    const auto child =
                        static_cast<std::size_t>(child_y) * fine.dimensions.width + child_x;
                    const float luminance_weight = fine.luminance_confidence[child];
                    const float chroma_weight = fine.chroma_confidence[child];
                    luminance_weight_sum += luminance_weight;
                    chroma_weight_sum += chroma_weight;
                    log_luminance_sum +=
                        static_cast<double>(fine.log_luminance[child]) * luminance_weight;
                    chroma_r_sum += static_cast<double>(fine.chroma_r[child]) * chroma_weight;
                    chroma_g_sum += static_cast<double>(fine.chroma_g[child]) * chroma_weight;
                }
            }
            const auto index = static_cast<std::size_t>(y) * dimensions.width + x;
            if (luminance_weight_sum > 0.0) {
                coarse.log_luminance[index] =
                    static_cast<float>(log_luminance_sum / luminance_weight_sum);
                coarse.luminance_confidence[index] =
                    std::min(1.0F, static_cast<float>(luminance_weight_sum / 4.0));
            }
            if (chroma_weight_sum > 0.0) {
                coarse.chroma_r[index] = static_cast<float>(chroma_r_sum / chroma_weight_sum);
                coarse.chroma_g[index] = static_cast<float>(chroma_g_sum / chroma_weight_sum);
                coarse.chroma_confidence[index] =
                    std::min(1.0F, static_cast<float>(chroma_weight_sum / 4.0));
            }
        }
    }
    return coarse;
}

[[nodiscard]] float bilinear_sample(
    const std::vector<float>& values,
    const Dimensions dimensions,
    const float x,
    const float y
) noexcept {
    const float clamped_x = std::clamp(x, 0.0F, static_cast<float>(dimensions.width - 1U));
    const float clamped_y = std::clamp(y, 0.0F, static_cast<float>(dimensions.height - 1U));
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const auto x1 = std::min(dimensions.width - 1U, x0 + 1U);
    const auto y1 = std::min(dimensions.height - 1U, y0 + 1U);
    const float tx = clamped_x - static_cast<float>(x0);
    const float ty = clamped_y - static_cast<float>(y0);
    const auto at = [&values,
                     dimensions](const std::uint32_t sample_x, const std::uint32_t sample_y) {
        return values[static_cast<std::size_t>(sample_y) * dimensions.width + sample_x];
    };
    const float upper = std::lerp(at(x0, y0), at(x1, y0), tx);
    const float lower = std::lerp(at(x0, y1), at(x1, y1), tx);
    return std::lerp(upper, lower, ty);
}

void pull_from_coarse(GuideLevel& fine, const GuideLevel& coarse) {
    for (std::uint32_t y = 0U; y < fine.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < fine.dimensions.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * fine.dimensions.width + x;
            const float coarse_x = (static_cast<float>(x) + 0.5F)
                                       * static_cast<float>(coarse.dimensions.width)
                                       / static_cast<float>(fine.dimensions.width)
                                   - 0.5F;
            const float coarse_y = (static_cast<float>(y) + 0.5F)
                                       * static_cast<float>(coarse.dimensions.height)
                                       / static_cast<float>(fine.dimensions.height)
                                   - 0.5F;
            const auto complete = [&](std::vector<float>& values,
                                      const std::vector<float>& parent,
                                      const float measured) {
                values[index] = std::lerp(
                    bilinear_sample(parent, coarse.dimensions, coarse_x, coarse_y),
                    values[index],
                    measured
                );
            };
            complete(
                fine.log_luminance,
                coarse.log_luminance,
                std::clamp(fine.luminance_confidence[index], 0.0F, 1.0F)
            );
            const float measured_chroma = std::clamp(fine.chroma_confidence[index], 0.0F, 1.0F);
            complete(fine.chroma_r, coarse.chroma_r, measured_chroma);
            complete(fine.chroma_g, coarse.chroma_g, measured_chroma);
            fine.luminance_confidence[index] = 1.0F;
            fine.chroma_confidence[index] = 1.0F;
        }
    }
}

void box_blur_once(
    std::vector<float>& values,
    const Dimensions dimensions,
    const std::uint32_t radius
) {
    if (radius == 0U) {
        return;
    }
    std::vector<float> temporary(values.size());
    std::vector<double> prefix(
        static_cast<std::size_t>(std::max(dimensions.width, dimensions.height)) + 1U
    );
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        prefix[0U] = 0.0;
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            prefix[x + 1U] = prefix[x] + values[static_cast<std::size_t>(y) * dimensions.width + x];
        }
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto begin = x > radius ? x - radius : 0U;
            const auto end = std::min(dimensions.width, x + radius + 1U);
            temporary[static_cast<std::size_t>(y) * dimensions.width + x] = static_cast<float>(
                (prefix[end] - prefix[begin]) / static_cast<double>(end - begin)
            );
        }
    }
    for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
        prefix[0U] = 0.0;
        for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
            prefix[y + 1U] =
                prefix[y] + temporary[static_cast<std::size_t>(y) * dimensions.width + x];
        }
        for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
            const auto begin = y > radius ? y - radius : 0U;
            const auto end = std::min(dimensions.height, y + radius + 1U);
            values[static_cast<std::size_t>(y) * dimensions.width + x] = static_cast<float>(
                (prefix[end] - prefix[begin]) / static_cast<double>(end - begin)
            );
        }
    }
}

void smooth_guide(
    std::vector<float>& values,
    const Dimensions dimensions,
    const std::uint32_t radius
) {
    box_blur_once(values, dimensions, radius);
    box_blur_once(values, dimensions, radius);
}

void smooth_weighted_guide(
    std::vector<float>& values,
    std::vector<float>& confidence,
    const Dimensions dimensions,
    const std::uint32_t radius
) {
    for (std::size_t index = 0U; index < values.size(); ++index) {
        values[index] *= confidence[index];
    }
    smooth_guide(values, dimensions, radius);
    smooth_guide(confidence, dimensions, radius);
    for (std::size_t index = 0U; index < values.size(); ++index) {
        values[index] =
            confidence[index] > minimum_luminance ? values[index] / confidence[index] : 0.0F;
    }
}

[[nodiscard]] std::vector<float>
prepare_clipping_influence(const SensorClippingMask& clipping, const Dimensions dimensions) {
    std::vector<float> influence(checked_count(dimensions));
    for (std::uint32_t guide_y = 0U; guide_y < dimensions.height; ++guide_y) {
        const auto source_y_begin =
            bin_begin(guide_y, clipping.dimensions.height, dimensions.height);
        const auto source_y_end = bin_end(guide_y, clipping.dimensions.height, dimensions.height);
        for (std::uint32_t guide_x = 0U; guide_x < dimensions.width; ++guide_x) {
            const auto source_x_begin =
                bin_begin(guide_x, clipping.dimensions.width, dimensions.width);
            const auto source_x_end = bin_end(guide_x, clipping.dimensions.width, dimensions.width);
            double influence_sum = 0.0;
            std::uint64_t total = 0U;
            for (std::uint32_t source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                for (std::uint32_t source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                    const auto pixel =
                        static_cast<std::size_t>(source_y) * clipping.dimensions.width + source_x;
                    // CFA risk describes colour trust, but its quantised levels are not spatial
                    // support. Only factual physical-clipping topology owns this continuous
                    // chroma-reconstruction feather.
                    influence_sum +=
                        (clipping.samples[pixel] & sensor_highlight_clipped) != 0U ? 1.0 : 0.0;
                    ++total;
                }
            }
            influence[static_cast<std::size_t>(guide_y) * dimensions.width + guide_x] =
                total == 0U ? 0.0F : static_cast<float>(influence_sum / static_cast<double>(total));
        }
    }
    const auto short_edge = std::min(dimensions.width, dimensions.height);
    const auto radius = std::clamp(short_edge / 72U, 2U, 6U);
    box_blur_once(influence, dimensions, radius);
    box_blur_once(influence, dimensions, radius);
    box_blur_once(influence, dimensions, radius);
    return influence;
}

[[nodiscard]] std::vector<float>
prepare_chroma_repair_influence(const HighlightChromaRiskMap& risk, const Dimensions dimensions) {
    std::vector<float> influence(checked_count(dimensions));
    for (std::uint32_t guide_y = 0U; guide_y < dimensions.height; ++guide_y) {
        const auto source_y_begin = bin_begin(guide_y, risk.dimensions.height, dimensions.height);
        const auto source_y_end = bin_end(guide_y, risk.dimensions.height, dimensions.height);
        for (std::uint32_t guide_x = 0U; guide_x < dimensions.width; ++guide_x) {
            const auto source_x_begin = bin_begin(guide_x, risk.dimensions.width, dimensions.width);
            const auto source_x_end = bin_end(guide_x, risk.dimensions.width, dimensions.width);
            double sum = 0.0;
            std::uint64_t total = 0U;
            for (std::uint32_t source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                for (std::uint32_t source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                    const auto pixel =
                        static_cast<std::size_t>(source_y) * risk.dimensions.width + source_x;
                    sum += static_cast<double>(risk.samples[pixel]) / 255.0;
                    ++total;
                }
            }
            influence[static_cast<std::size_t>(guide_y) * dimensions.width + guide_x] =
                total == 0U ? 0.0F : static_cast<float>(sum / static_cast<double>(total));
        }
    }
    const auto short_edge = std::min(dimensions.width, dimensions.height);
    const auto radius = std::clamp(short_edge / 96U, 1U, 4U);
    smooth_guide(influence, dimensions, radius);
    return influence;
}

} // namespace

ClippedHighlightReconstructionStats reconstruct_clipped_highlight_surface(
    SceneLinearRgbFrame& scene_linear,
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk
) {
    if (!scene_linear.valid() || !sensor_clipping.valid()
        || sensor_clipping.dimensions != scene_linear.dimensions || !highlight_chroma_risk.valid()
        || highlight_chroma_risk.dimensions != scene_linear.dimensions) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "clipped-highlight reconstruction requires aligned valid scene-linear source evidence"
        );
    }
    ClippedHighlightReconstructionStats stats{
        .clipped_pixel_count = sensor_clipping.highlight_pixel_count,
    };
    if (stats.clipped_pixel_count == 0U
        || stats.clipped_pixel_count == scene_linear.dimensions.pixel_count()) {
        return stats;
    }
    highlight_chroma_risk.source_surface_reconstructed = true;

    stats.guide_dimensions = guide_dimensions(scene_linear.dimensions);
    std::vector<GuideLevel> pyramid;
    pyramid.push_back(prepare_finest_guide(
        scene_linear,
        sensor_clipping,
        highlight_chroma_risk,
        stats.guide_dimensions
    ));
    while (pyramid.back().dimensions.width > 1U || pyramid.back().dimensions.height > 1U) {
        pyramid.push_back(reduce_guide(pyramid.back()));
    }
    if (pyramid.back().luminance_confidence[0U] <= 0.0F
        || pyramid.back().chroma_confidence[0U] <= 0.0F) {
        return stats;
    }
    for (std::size_t level = pyramid.size() - 1U; level > 0U; --level) {
        pull_from_coarse(pyramid[level - 1U], pyramid[level]);
    }
    GuideLevel& guide = pyramid.front();
    const auto guide_short_edge = std::min(guide.dimensions.width, guide.dimensions.height);
    const auto guide_smoothing_radius = std::clamp(guide_short_edge / 96U, 1U, 4U);
    smooth_guide(guide.log_luminance, guide.dimensions, guide_smoothing_radius);
    const auto boundary_luminance_radius = std::clamp(guide_short_edge / 32U, 3U, 12U);
    smooth_weighted_guide(
        guide.observed_log_luminance,
        guide.observed_luminance_confidence,
        guide.dimensions,
        boundary_luminance_radius
    );
    std::vector<float> observed_chroma_g_confidence = guide.observed_chroma_confidence;
    smooth_weighted_guide(
        guide.observed_chroma_r,
        guide.observed_chroma_confidence,
        guide.dimensions,
        boundary_luminance_radius
    );
    smooth_weighted_guide(
        guide.observed_chroma_g,
        observed_chroma_g_confidence,
        guide.dimensions,
        boundary_luminance_radius
    );
    smooth_guide(guide.chroma_r, guide.dimensions, guide_smoothing_radius);
    smooth_guide(guide.chroma_g, guide.dimensions, guide_smoothing_radius);
    const std::vector<float> influence =
        prepare_clipping_influence(sensor_clipping, guide.dimensions);
    const std::vector<float> chroma_repair_influence =
        prepare_chroma_repair_influence(highlight_chroma_risk, guide.dimensions);

    for (std::uint32_t y = 0U; y < scene_linear.dimensions.height; ++y) {
        const float guide_y = (static_cast<float>(y) + 0.5F)
                                  * static_cast<float>(guide.dimensions.height)
                                  / static_cast<float>(scene_linear.dimensions.height)
                              - 0.5F;
        for (std::uint32_t x = 0U; x < scene_linear.dimensions.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * scene_linear.dimensions.width + x;
            const bool clipped = (sensor_clipping.samples[pixel] & sensor_highlight_clipped) != 0U;
            const float guide_x = (static_cast<float>(x) + 0.5F)
                                      * static_cast<float>(guide.dimensions.width)
                                      / static_cast<float>(scene_linear.dimensions.width)
                                  - 0.5F;
            const float local_influence =
                bilinear_sample(influence, guide.dimensions, guide_x, guide_y);
            const float local_chroma_risk =
                bilinear_sample(chroma_repair_influence, guide.dimensions, guide_x, guide_y);
            if (!clipped && local_influence < 0.005F && local_chroma_risk < 0.005F) {
                continue;
            }

            const auto sample = pixel * 3U;
            const float input_r = scene_linear.samples[sample];
            const float input_g = scene_linear.samples[sample + 1U];
            const float input_b = scene_linear.samples[sample + 2U];
            const float input_luminance = positive_luminance(input_r, input_g, input_b);
            const float guide_luminance =
                std::exp2(bilinear_sample(guide.log_luminance, guide.dimensions, guide_x, guide_y));
            const float observed_smooth_luminance = std::exp2(
                bilinear_sample(guide.observed_log_luminance, guide.dimensions, guide_x, guide_y)
            );
            float target_chroma_r = std::clamp(
                bilinear_sample(guide.observed_chroma_r, guide.dimensions, guide_x, guide_y),
                0.0F,
                1.0F
            );
            float target_chroma_g = std::clamp(
                bilinear_sample(guide.observed_chroma_g, guide.dimensions, guide_x, guide_y),
                0.0F,
                1.0F
            );
            float target_chroma_b = std::max(0.0F, 1.0F - target_chroma_r - target_chroma_g);
            const float chroma_sum =
                std::max(target_chroma_r + target_chroma_g + target_chroma_b, minimum_luminance);
            target_chroma_r /= chroma_sum;
            target_chroma_g /= chroma_sum;
            target_chroma_b /= chroma_sum;
            float reliable_chroma_r = std::clamp(
                bilinear_sample(guide.chroma_r, guide.dimensions, guide_x, guide_y),
                0.0F,
                1.0F
            );
            float reliable_chroma_g = std::clamp(
                bilinear_sample(guide.chroma_g, guide.dimensions, guide_x, guide_y),
                0.0F,
                1.0F
            );
            float reliable_chroma_b = std::max(0.0F, 1.0F - reliable_chroma_r - reliable_chroma_g);
            const float reliable_chroma_sum = std::max(
                reliable_chroma_r + reliable_chroma_g + reliable_chroma_b,
                minimum_luminance
            );
            reliable_chroma_r /= reliable_chroma_sum;
            reliable_chroma_g /= reliable_chroma_sum;
            reliable_chroma_b /= reliable_chroma_sum;
            const float chroma_disagreement = std::abs(target_chroma_r - reliable_chroma_r)
                                              + std::abs(target_chroma_g - reliable_chroma_g)
                                              + std::abs(target_chroma_b - reliable_chroma_b);
            const float deep_core = smoothstep(0.58F, 0.92F, local_influence);
            // Observed colour keeps the local contour continuous, but high CFA risk means those
            // ratios are no longer trustworthy evidence. Replace their direction with the nearby
            // measured, unclipped and low-risk colour at the clipping boundary. The deep core does
            // not import that potentially distant guide wholesale; it retains more of its locally
            // smoothed colour unless the two sources materially disagree.
            const float reliable_chroma_blend = 0.92F * smoothstep(0.06F, 0.78F, local_chroma_risk)
                                                * std::lerp(1.0F, 0.28F, deep_core);
            target_chroma_r = std::lerp(target_chroma_r, reliable_chroma_r, reliable_chroma_blend);
            target_chroma_g = std::lerp(target_chroma_g, reliable_chroma_g, reliable_chroma_blend);
            target_chroma_b = std::lerp(target_chroma_b, reliable_chroma_b, reliable_chroma_blend);
            // If a terminal core's local colour contradicts the measured boundary, neither hue is
            // factual enough to justify a coloured plateau. Desaturate only that contradictory
            // component. Agreement leaves a genuinely warm emitter untouched.
            const float core_unreliability =
                std::max(smoothstep(0.06F, 0.78F, local_chroma_risk), clipped ? 1.0F : 0.0F);
            const float core_neutral_pull = 0.94F * deep_core
                                            * smoothstep(0.08F, 0.36F, chroma_disagreement)
                                            * core_unreliability;
            target_chroma_r = std::lerp(target_chroma_r, 1.0F / 3.0F, core_neutral_pull);
            target_chroma_g = std::lerp(target_chroma_g, 1.0F / 3.0F, core_neutral_pull);
            target_chroma_b = std::lerp(target_chroma_b, 1.0F / 3.0F, core_neutral_pull);
            // The physical boundary's continuous influence bell adopts reliable neighbouring
            // luminance, while the unknowable deep core adopts only the broader locally observed
            // light surface. This removes the 99/100 platform edge and quantised core plateaus
            // without importing a distant guide colour or touching the far measured exterior.
            const float bounded_influence = std::clamp(local_influence, 0.0F, 1.0F);
            const float boundary_luminance_weight =
                std::sqrt(std::max(0.0F, 4.0F * bounded_influence * (1.0F - bounded_influence)));
            const float reliable_boundary_luminance =
                std::lerp(observed_smooth_luminance, guide_luminance, 0.82F);
            const float boundary_luminance =
                std::lerp(input_luminance, reliable_boundary_luminance, boundary_luminance_weight);
            const float core_luminance_blend = clipped ? 0.92F * deep_core : 0.0F;
            const float reconstructed_core_luminance =
                std::lerp(boundary_luminance, observed_smooth_luminance, core_luminance_blend);
            const float target_luminance = clipped ? reconstructed_core_luminance
                                                   : std::max(boundary_luminance, input_luminance);
            const float target_unit_luminance = target_chroma_r * luminance_weights[0U]
                                                + target_chroma_g * luminance_weights[1U]
                                                + target_chroma_b * luminance_weights[2U];
            const float target_scale =
                target_luminance / std::max(target_unit_luminance, minimum_luminance);
            const std::array<float, 3U> target{
                target_chroma_r * target_scale,
                target_chroma_g * target_scale,
                target_chroma_b * target_scale,
            };

            // A symmetric blur is roughly 0.5 at the boundary of a coherent clipped surface. This
            // bell peaks only at that frontier; the separate core term above owns deep physical
            // clipping. Sparse CFA hits are suppressed so isolated edge hairs cannot become
            // colour-smearing seeds.
            const float coherent_support = smoothstep(
                surface_reconstruction_support_begin,
                surface_reconstruction_support_full,
                local_influence
            );
            const float boundary_blend = boundary_luminance_weight * coherent_support;
            // Risk can be spatially projected onto a dark fixture next to a clipped lamp. The
            // repair is allowed to cross a highlight contour, but never a genuinely dark edge.
            const float observed_brightness_ratio =
                input_luminance / std::max(observed_smooth_luminance, minimum_luminance);
            const float observed_edge_gate = smoothstep(0.86F, 0.995F, observed_brightness_ratio);
            const float chroma_repair_brightness_gate =
                smoothstep(0.32F, 0.78F, input_luminance) * observed_edge_gate;
            const float chroma_repair_blend =
                smoothstep(0.03F, 0.72F, local_chroma_risk) * chroma_repair_brightness_gate;
            const float spatial_blend =
                std::max(boundary_blend * observed_edge_gate, chroma_repair_blend);
            // Projection bins can straddle a clipped lamp and a dark fixture. Even when such a
            // pixel carries factual clipping and coherent mask support, its repaired RGB remains
            // strong ownership evidence for the dark edge. Dense bright cores pass the looser
            // clipped threshold; an exterior shoulder still uses the stricter measured-light
            // threshold.
            const float brightness_ratio =
                input_luminance / std::max(guide_luminance, minimum_luminance);
            const float brightness_gate = clipped ? smoothstep(0.45F, 0.85F, brightness_ratio)
                                                  : smoothstep(0.50F, 0.92F, brightness_ratio);
            const float blend = spatial_blend * brightness_gate;
            if (blend <= 0.0F) {
                continue;
            }
            float output_r = std::lerp(input_r, target[0U], blend);
            float output_g = std::lerp(input_g, target[1U], blend);
            float output_b = std::lerp(input_b, target[2U], blend);
            // Chroma-risk topology must not become a second luminance topology. Reproject the
            // blended colour onto the exact interpolated luminance so differently quantised risk
            // bands remain brightness-identical when the physical clipping influence is equal.
            const float luminance_blend = std::max(boundary_blend, core_luminance_blend)
                                          * observed_edge_gate * brightness_gate;
            const float intended_luminance =
                std::lerp(input_luminance, target_luminance, luminance_blend);
            const float output_luminance = positive_luminance(output_r, output_g, output_b);
            const float luminance_scale = intended_luminance / output_luminance;
            output_r *= luminance_scale;
            output_g *= luminance_scale;
            output_b *= luminance_scale;
            scene_linear.samples[sample] = output_r;
            scene_linear.samples[sample + 1U] = output_g;
            scene_linear.samples[sample + 2U] = output_b;
            // Remove only the fraction of CFA colour risk that this reconstruction actually
            // replaced. Hard-clearing every touched pixel creates a new 0/risk boundary at the
            // feather edge; strong recovery then reveals that boundary as a colour ring.
            const float remaining_risk =
                static_cast<float>(highlight_chroma_risk.samples[pixel]) * (1.0F - blend);
            highlight_chroma_risk.samples[pixel] =
                static_cast<std::uint8_t>(std::clamp(std::lround(remaining_risk), 0L, 255L));
            ++stats.blended_pixel_count;
        }
    }
    if (!scene_linear.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "clipped-highlight reconstruction produced a non-finite scene-linear surface"
        );
    }
    return stats;
}

} // namespace shadow::image::raw_pipeline_detail
