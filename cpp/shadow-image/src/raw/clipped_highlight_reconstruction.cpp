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
// Physical clipping destroys colour and texture, not the fact that the core was bright. Permit the
// low-frequency guide to remove at most one quarter stop from measured clipped luminance.
constexpr float minimum_clipped_luminance_retention = 0.84089642F;
constexpr std::array<float, 3U> luminance_weights{0.2126F, 0.7152F, 0.0722F};

struct GuideLevel final {
    Dimensions dimensions;
    std::vector<float> log_luminance;
    std::vector<float> chroma_r;
    std::vector<float> chroma_g;
    std::vector<float> confidence;
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
    const Dimensions dimensions
) {
    GuideLevel guide{
        .dimensions = dimensions,
        .log_luminance = std::vector<float>(checked_count(dimensions)),
        .chroma_r = std::vector<float>(checked_count(dimensions)),
        .chroma_g = std::vector<float>(checked_count(dimensions)),
        .confidence = std::vector<float>(checked_count(dimensions)),
    };
    for (std::uint32_t guide_y = 0U; guide_y < dimensions.height; ++guide_y) {
        const auto source_y_begin = bin_begin(guide_y, source.dimensions.height, dimensions.height);
        const auto source_y_end = bin_end(guide_y, source.dimensions.height, dimensions.height);
        for (std::uint32_t guide_x = 0U; guide_x < dimensions.width; ++guide_x) {
            const auto source_x_begin =
                bin_begin(guide_x, source.dimensions.width, dimensions.width);
            const auto source_x_end = bin_end(guide_x, source.dimensions.width, dimensions.width);
            double log_luminance_sum = 0.0;
            double chroma_r_sum = 0.0;
            double chroma_g_sum = 0.0;
            double measured_weight = 0.0;
            std::uint64_t total_count = 0U;
            for (std::uint32_t source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                for (std::uint32_t source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                    const auto pixel =
                        static_cast<std::size_t>(source_y) * source.dimensions.width + source_x;
                    ++total_count;
                    if ((clipping.samples[pixel] & sensor_highlight_clipped) != 0U) {
                        continue;
                    }
                    const auto sample = pixel * 3U;
                    const float red = std::max(0.0F, source.samples[sample]);
                    const float green = std::max(0.0F, source.samples[sample + 1U]);
                    const float blue = std::max(0.0F, source.samples[sample + 2U]);
                    const float luminance = positive_luminance(red, green, blue);
                    const float sum = std::max(red + green + blue, minimum_luminance);
                    // A physical highlight may border a dark silhouette. That silhouette is
                    // trustworthy image content but not a plausible continuation of the lost
                    // highlight surface, so admit measured guide samples continuously by their
                    // scene-linear highlight luminance instead of letting a black edge pull the
                    // entire reconstruction down.
                    const float weight = smoothstep(0.18F, 0.75F, luminance);
                    log_luminance_sum += static_cast<double>(std::log2(luminance)) * weight;
                    chroma_r_sum += static_cast<double>(red / sum) * weight;
                    chroma_g_sum += static_cast<double>(green / sum) * weight;
                    measured_weight += weight;
                }
            }
            const auto index = static_cast<std::size_t>(guide_y) * dimensions.width + guide_x;
            if (measured_weight > 0.0) {
                guide.log_luminance[index] =
                    static_cast<float>(log_luminance_sum / measured_weight);
                guide.chroma_r[index] = static_cast<float>(chroma_r_sum / measured_weight);
                guide.chroma_g[index] = static_cast<float>(chroma_g_sum / measured_weight);
                guide.confidence[index] = std::min(
                    1.0F,
                    static_cast<float>(measured_weight / static_cast<double>(total_count))
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
        .chroma_r = std::vector<float>(checked_count(dimensions)),
        .chroma_g = std::vector<float>(checked_count(dimensions)),
        .confidence = std::vector<float>(checked_count(dimensions)),
    };
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            double weight_sum = 0.0;
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
                    const float weight = fine.confidence[child];
                    weight_sum += weight;
                    log_luminance_sum += static_cast<double>(fine.log_luminance[child]) * weight;
                    chroma_r_sum += static_cast<double>(fine.chroma_r[child]) * weight;
                    chroma_g_sum += static_cast<double>(fine.chroma_g[child]) * weight;
                }
            }
            const auto index = static_cast<std::size_t>(y) * dimensions.width + x;
            if (weight_sum > 0.0) {
                coarse.log_luminance[index] = static_cast<float>(log_luminance_sum / weight_sum);
                coarse.chroma_r[index] = static_cast<float>(chroma_r_sum / weight_sum);
                coarse.chroma_g[index] = static_cast<float>(chroma_g_sum / weight_sum);
                coarse.confidence[index] = std::min(1.0F, static_cast<float>(weight_sum / 4.0));
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
            const float measured = std::clamp(fine.confidence[index], 0.0F, 1.0F);
            const auto complete = [&](std::vector<float>& values,
                                      const std::vector<float>& parent) {
                values[index] = std::lerp(
                    bilinear_sample(parent, coarse.dimensions, coarse_x, coarse_y),
                    values[index],
                    measured
                );
            };
            complete(fine.log_luminance, coarse.log_luminance);
            complete(fine.chroma_r, coarse.chroma_r);
            complete(fine.chroma_g, coarse.chroma_g);
            fine.confidence[index] = 1.0F;
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
            std::uint64_t clipped = 0U;
            std::uint64_t total = 0U;
            for (std::uint32_t source_y = source_y_begin; source_y < source_y_end; ++source_y) {
                for (std::uint32_t source_x = source_x_begin; source_x < source_x_end; ++source_x) {
                    const auto pixel =
                        static_cast<std::size_t>(source_y) * clipping.dimensions.width + source_x;
                    clipped += (clipping.samples[pixel] & sensor_highlight_clipped) != 0U ? 1U : 0U;
                    ++total;
                }
            }
            influence[static_cast<std::size_t>(guide_y) * dimensions.width + guide_x] =
                total == 0U ? 0.0F : static_cast<float>(clipped) / static_cast<float>(total);
        }
    }
    const auto short_edge = std::min(dimensions.width, dimensions.height);
    const auto radius = std::clamp(short_edge / 72U, 2U, 6U);
    box_blur_once(influence, dimensions, radius);
    box_blur_once(influence, dimensions, radius);
    box_blur_once(influence, dimensions, radius);
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
    pyramid.push_back(prepare_finest_guide(scene_linear, sensor_clipping, stats.guide_dimensions));
    while (pyramid.back().dimensions.width > 1U || pyramid.back().dimensions.height > 1U) {
        pyramid.push_back(reduce_guide(pyramid.back()));
    }
    if (pyramid.back().confidence[0U] <= 0.0F) {
        return stats;
    }
    for (std::size_t level = pyramid.size() - 1U; level > 0U; --level) {
        pull_from_coarse(pyramid[level - 1U], pyramid[level]);
    }
    GuideLevel& guide = pyramid.front();
    const auto guide_short_edge = std::min(guide.dimensions.width, guide.dimensions.height);
    const auto guide_smoothing_radius = std::clamp(guide_short_edge / 96U, 1U, 4U);
    smooth_guide(guide.log_luminance, guide.dimensions, guide_smoothing_radius);
    smooth_guide(guide.chroma_r, guide.dimensions, guide_smoothing_radius);
    smooth_guide(guide.chroma_g, guide.dimensions, guide_smoothing_radius);
    const std::vector<float> influence =
        prepare_clipping_influence(sensor_clipping, guide.dimensions);

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
            if (!clipped && local_influence < 0.005F) {
                continue;
            }

            const auto sample = pixel * 3U;
            const float input_r = scene_linear.samples[sample];
            const float input_g = scene_linear.samples[sample + 1U];
            const float input_b = scene_linear.samples[sample + 2U];
            const float input_luminance = positive_luminance(input_r, input_g, input_b);
            const float guide_luminance =
                std::exp2(bilinear_sample(guide.log_luminance, guide.dimensions, guide_x, guide_y));
            float target_chroma_r = std::clamp(
                bilinear_sample(guide.chroma_r, guide.dimensions, guide_x, guide_y),
                0.0F,
                1.0F
            );
            float target_chroma_g = std::clamp(
                bilinear_sample(guide.chroma_g, guide.dimensions, guide_x, guide_y),
                0.0F,
                1.0F
            );
            float target_chroma_b = std::max(0.0F, 1.0F - target_chroma_r - target_chroma_g);
            const float chroma_sum =
                std::max(target_chroma_r + target_chroma_g + target_chroma_b, minimum_luminance);
            target_chroma_r /= chroma_sum;
            target_chroma_g /= chroma_sum;
            target_chroma_b /= chroma_sum;
            // The measured boundary is the best available colour evidence. Retain its warmth and
            // use only a small neutral safety pull in the unknowable centre; the earlier CFA stage
            // has already removed channel-clipping false colour.
            const float neutral_pull = 0.12F * smoothstep(0.78F, 0.98F, local_influence);
            target_chroma_r = std::lerp(target_chroma_r, 1.0F / 3.0F, neutral_pull);
            target_chroma_g = std::lerp(target_chroma_g, 1.0F / 3.0F, neutral_pull);
            target_chroma_b = std::lerp(target_chroma_b, 1.0F / 3.0F, neutral_pull);
            // Do not let reconstruction become a second highlight tone mapper. The clipped core
            // may follow a brighter low-frequency guide, but it cannot be pulled down by more than
            // a quarter stop. The exterior shoulder changes chromaticity at constant luminance.
            const float target_luminance = clipped
                                               ? std::max(
                                                     guide_luminance,
                                                     input_luminance
                                                         * minimum_clipped_luminance_retention
                                                 )
                                               : input_luminance;
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

            // A symmetric blur is roughly 0.5 at the binary mask boundary. Renormalize its
            // exterior half so the first measured highlight pixel meets the fully reconstructed
            // clipped pixel continuously; the later brightness gate still rejects a dark edge.
            const float blend_influence = clipped ? 1.0F : std::min(1.0F, local_influence * 2.0F);
            const float spatial_blend = smoothstep(0.02F, 0.98F, blend_influence);
            float blend = spatial_blend;
            if (!clipped) {
                const float brightness_gate = smoothstep(
                    0.45F,
                    0.90F,
                    input_luminance / std::max(guide_luminance, minimum_luminance)
                );
                blend = spatial_blend * brightness_gate;
            }
            if (blend <= 0.0F) {
                continue;
            }
            scene_linear.samples[sample] = std::lerp(input_r, target[0U], blend);
            scene_linear.samples[sample + 1U] = std::lerp(input_g, target[1U], blend);
            scene_linear.samples[sample + 2U] = std::lerp(input_b, target[2U], blend);
            highlight_chroma_risk.samples[pixel] = static_cast<std::uint8_t>(std::lround(
                static_cast<float>(highlight_chroma_risk.samples[pixel]) * (1.0F - blend)
            ));
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
