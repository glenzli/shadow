#include "clipped_highlight_reconstruction.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>

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
// white, but that is not evidence that the whole RGB surface is missing. The surface influence is
// therefore seeded only by the mask's shared three-colour terminal core, then requires spatial
// coherence before replacing luminance. This leaves single-channel saturated light measured and
// tapers isolated Bayer-phase hits instead of growing edge hairs.
constexpr float surface_reconstruction_support_begin = 0.035F;
constexpr float surface_reconstruction_support_full = 0.20F;
constexpr std::array<float, 3U> luminance_weights{0.2126F, 0.7152F, 0.0722F};

struct GuideLevel final {
    Dimensions dimensions;
    std::vector<float> log_luminance;
    std::vector<float> observed_log_luminance;
    std::vector<float> observed_luminance_confidence;
    std::vector<float> chroma_r;
    std::vector<float> chroma_g;
    std::vector<float> luminance_confidence;
    std::vector<float> chroma_confidence;
};

struct CameraSpaceMatrices final {
    std::array<double, 9U> camera_to_scene{
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    std::array<double, 9U> scene_to_camera = camera_to_scene;
};

[[nodiscard]] std::array<float, 3U> transform_rgb(
    const std::array<double, 9U>& matrix,
    const float red,
    const float green,
    const float blue
) noexcept {
    return {
        static_cast<float>(matrix[0U] * red + matrix[1U] * green + matrix[2U] * blue),
        static_cast<float>(matrix[3U] * red + matrix[4U] * green + matrix[5U] * blue),
        static_cast<float>(matrix[6U] * red + matrix[7U] * green + matrix[8U] * blue),
    };
}

[[nodiscard]] CameraSpaceMatrices
camera_space_matrices(const RawFrameLinearTransform* const transform) {
    CameraSpaceMatrices result;
    if (transform == nullptr) {
        return result;
    }
    result.camera_to_scene = transform->camera_to_linear_srgb_d65;
    const auto& m = result.camera_to_scene;
    const double determinant = m[0U] * (m[4U] * m[8U] - m[5U] * m[7U])
                               - m[1U] * (m[3U] * m[8U] - m[5U] * m[6U])
                               + m[2U] * (m[3U] * m[7U] - m[4U] * m[6U]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "clipped-highlight reconstruction requires an invertible camera transform"
        );
    }
    const double reciprocal = 1.0 / determinant;
    result.scene_to_camera = {
        (m[4U] * m[8U] - m[5U] * m[7U]) * reciprocal,
        (m[2U] * m[7U] - m[1U] * m[8U]) * reciprocal,
        (m[1U] * m[5U] - m[2U] * m[4U]) * reciprocal,
        (m[5U] * m[6U] - m[3U] * m[8U]) * reciprocal,
        (m[0U] * m[8U] - m[2U] * m[6U]) * reciprocal,
        (m[2U] * m[3U] - m[0U] * m[5U]) * reciprocal,
        (m[3U] * m[7U] - m[4U] * m[6U]) * reciprocal,
        (m[1U] * m[6U] - m[0U] * m[7U]) * reciprocal,
        (m[0U] * m[4U] - m[1U] * m[3U]) * reciprocal,
    };
    return result;
}

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

[[nodiscard]] float native_chroma_risk_opacity(
    const HighlightChromaRiskMap& risk,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    // Risk is classified directly from the CFA footprint represented by this output pixel. Never
    // dilate it here: an earlier spatial feather hid the last magenta row only by painting a new
    // yellow/grey row outside the damaged footprint. The low-resolution guide may look farther
    // away when choosing a replacement colour, but write ownership remains native-resolution.
    return static_cast<float>(risk.samples[static_cast<std::size_t>(y) * risk.dimensions.width + x])
           / 255.0F;
}

[[nodiscard]] float scene_referred_terminal_shoulder(
    const float observed_luminance,
    const float measured_boundary_luminance
) noexcept {
    const float boundary = std::max(measured_boundary_luminance, minimum_luminance);
    const float observed = std::max(observed_luminance, boundary);
    const float headroom = observed - boundary;
    // A logarithmic shoulder is identity-valued with unit slope at the measured boundary, remains
    // monotonic for arbitrarily large WB gains, and retains materially more light than projecting
    // physical white to 1.0. Its scale is local, so different camera matrices and WB settings do
    // not introduce a new global source ceiling.
    const float scale = std::max(0.18F, boundary * 0.65F);
    return boundary + scale * std::log1p(headroom / scale);
}

[[nodiscard]] GuideLevel prepare_finest_guide(
    const SceneLinearRgbFrame& source,
    const SensorClippingMask& clipping,
    const HighlightChromaRiskMap& highlight_chroma_risk,
    const Dimensions dimensions,
    const CameraSpaceMatrices& matrices
) {
    GuideLevel guide{
        .dimensions = dimensions,
        .log_luminance = std::vector<float>(checked_count(dimensions)),
        .observed_log_luminance = std::vector<float>(checked_count(dimensions)),
        .observed_luminance_confidence = std::vector<float>(checked_count(dimensions)),
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
                    const auto camera = transform_rgb(
                        matrices.scene_to_camera,
                        source.samples[sample],
                        source.samples[sample + 1U],
                        source.samples[sample + 2U]
                    );
                    const float camera_red = std::max(0.0F, camera[0U]);
                    const float camera_green = std::max(0.0F, camera[1U]);
                    const float camera_blue = std::max(0.0F, camera[2U]);
                    const float sum =
                        std::max(camera_red + camera_green + camera_blue, minimum_luminance);
                    // A physical highlight may border a dark silhouette. That silhouette is
                    // trustworthy image content but not a plausible continuation of the lost
                    // highlight surface, so admit measured guide samples continuously by their
                    // scene-linear highlight luminance instead of letting a black edge pull the
                    // entire reconstruction down.
                    chroma_r_sum += static_cast<double>(camera_red / sum) * chroma_weight;
                    chroma_g_sum += static_cast<double>(camera_green / sum) * chroma_weight;
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
                    // support. Only the exact fraction of the CFA footprint with three-colour
                    // physical clipping owns this continuous reconstruction influence.
                    influence_sum += clipping.shared_highlight_coverage_at(pixel);
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

void propagate_boundary_chroma_inside_clipping(
    GuideLevel& guide,
    const std::vector<float>& measured_chroma_r,
    const std::vector<float>& measured_chroma_g,
    const std::vector<float>& measured_chroma_confidence,
    const std::vector<float>& clipping_influence
) {
    // Preserve which guide cells are genuine camera-space observations. The pyramid supplies a
    // cheap initial value for every missing cell, but must not turn those inferred values into new
    // colour seeds. Jacobi relaxation then propagates the fixed measured boundary inward through
    // only the connected clipping support. This is the important separation used by segmented
    // highlight reconstruction: spatial ownership is fixed by clipping topology while colour is
    // solved inside it, rather than painting a blurred boundary colour across both sides.
    constexpr std::uint32_t iteration_count = 24U;
    std::vector<float> next_r(guide.chroma_r.size());
    std::vector<float> next_g(guide.chroma_g.size());
    for (std::uint32_t iteration = 0U; iteration < iteration_count; ++iteration) {
        next_r = guide.chroma_r;
        next_g = guide.chroma_g;
        for (std::uint32_t y = 0U; y < guide.dimensions.height; ++y) {
            for (std::uint32_t x = 0U; x < guide.dimensions.width; ++x) {
                const auto index = static_cast<std::size_t>(y) * guide.dimensions.width + x;
                if (clipping_influence[index] < 0.002F) {
                    continue;
                }
                const float measured = std::clamp(measured_chroma_confidence[index], 0.0F, 1.0F);
                if (measured >= 0.995F) {
                    next_r[index] = measured_chroma_r[index];
                    next_g[index] = measured_chroma_g[index];
                    continue;
                }
                float neighbour_r = 0.0F;
                float neighbour_g = 0.0F;
                float neighbour_weight = 0.0F;
                const auto accumulate = [&](const std::uint32_t sample_x,
                                            const std::uint32_t sample_y,
                                            const float weight) {
                    const auto neighbour =
                        static_cast<std::size_t>(sample_y) * guide.dimensions.width + sample_x;
                    neighbour_r += guide.chroma_r[neighbour] * weight;
                    neighbour_g += guide.chroma_g[neighbour] * weight;
                    neighbour_weight += weight;
                };
                if (x > 0U) {
                    accumulate(x - 1U, y, 1.0F);
                }
                if (x + 1U < guide.dimensions.width) {
                    accumulate(x + 1U, y, 1.0F);
                }
                if (y > 0U) {
                    accumulate(x, y - 1U, 1.0F);
                }
                if (y + 1U < guide.dimensions.height) {
                    accumulate(x, y + 1U, 1.0F);
                }
                if (neighbour_weight <= 0.0F) {
                    continue;
                }
                neighbour_r /= neighbour_weight;
                neighbour_g /= neighbour_weight;
                // A partially measured cell remains a soft boundary constraint. Its actual
                // sample is never replaced by another inferred cell, while low-confidence bins
                // are free to converge to the harmonic continuation of nearby measured colour.
                const float boundary_anchor = smoothstep(0.02F, 0.72F, measured);
                next_r[index] = std::lerp(neighbour_r, measured_chroma_r[index], boundary_anchor);
                next_g[index] = std::lerp(neighbour_g, measured_chroma_g[index], boundary_anchor);
            }
        }
        guide.chroma_r.swap(next_r);
        guide.chroma_g.swap(next_g);
    }
}

} // namespace

void complete_cfa_owned_highlight_reconstruction(HighlightChromaRiskMap& highlight_chroma_risk) {
    if (!highlight_chroma_risk.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "completed CFA-owned highlight reconstruction requires a valid risk map"
        );
    }
    highlight_chroma_risk.source_surface_reconstructed = true;
    std::fill(highlight_chroma_risk.samples.begin(), highlight_chroma_risk.samples.end(), 0U);
}

ClippedHighlightReconstructionStats reconstruct_clipped_highlight_surface(
    SceneLinearRgbFrame& scene_linear,
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk,
    const RawFrameLinearTransform* const camera_transform
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
    const CameraSpaceMatrices matrices = camera_space_matrices(camera_transform);

    stats.guide_dimensions = guide_dimensions(scene_linear.dimensions);
    std::vector<GuideLevel> pyramid;
    pyramid.push_back(prepare_finest_guide(
        scene_linear,
        sensor_clipping,
        highlight_chroma_risk,
        stats.guide_dimensions,
        matrices
    ));
    const std::vector<float> measured_chroma_r = pyramid.front().chroma_r;
    const std::vector<float> measured_chroma_g = pyramid.front().chroma_g;
    const std::vector<float> measured_chroma_confidence = pyramid.front().chroma_confidence;
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
    const std::vector<float> influence =
        prepare_clipping_influence(sensor_clipping, guide.dimensions);
    propagate_boundary_chroma_inside_clipping(
        guide,
        measured_chroma_r,
        measured_chroma_g,
        measured_chroma_confidence,
        influence
    );

    for (std::uint32_t y = 0U; y < scene_linear.dimensions.height; ++y) {
        const float guide_y = (static_cast<float>(y) + 0.5F)
                                  * static_cast<float>(guide.dimensions.height)
                                  / static_cast<float>(scene_linear.dimensions.height)
                              - 0.5F;
        for (std::uint32_t x = 0U; x < scene_linear.dimensions.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * scene_linear.dimensions.width + x;
            const auto clipping_flags = sensor_clipping.samples[pixel];
            const bool clipped = (clipping_flags & sensor_highlight_clipped) != 0U;
            const float shared_clipped_coverage =
                sensor_clipping.shared_highlight_coverage_at(pixel);
            const float guide_x = (static_cast<float>(x) + 0.5F)
                                      * static_cast<float>(guide.dimensions.width)
                                      / static_cast<float>(scene_linear.dimensions.width)
                                  - 0.5F;
            const float local_influence =
                bilinear_sample(influence, guide.dimensions, guide_x, guide_y);
            const float local_chroma_risk = native_chroma_risk_opacity(highlight_chroma_risk, x, y);
            // Neighbouring risk remains useful for rejecting corrupt guide seeds, but never owns a
            // scene-RGB write. CFA/area opposed repair handles a response-shoulder boundary before
            // the camera matrix. This source-surface pass writes only the factual physical-clipping
            // mask, matching darktable's rule that dilation gathers candidates but does not enlarge
            // damaged-site replacement.
            if (!clipped || shared_clipped_coverage <= 0.0F) {
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
            const auto input_camera =
                transform_rgb(matrices.scene_to_camera, input_r, input_g, input_b);
            const float input_camera_r = std::max(0.0F, input_camera[0U]);
            const float input_camera_g = std::max(0.0F, input_camera[1U]);
            const float input_camera_b = std::max(0.0F, input_camera[2U]);
            const float input_chroma_sum =
                std::max(input_camera_r + input_camera_g + input_camera_b, minimum_luminance);
            float target_chroma_r = std::clamp(input_camera_r / input_chroma_sum, 0.0F, 1.0F);
            float target_chroma_g = std::clamp(input_camera_g / input_chroma_sum, 0.0F, 1.0F);
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
            // The guide now contains a boundary-anchored camera-space continuation only inside the
            // clipped segment. It removes internal CFA colour islands without borrowing a flat hue
            // from a search circle or expanding that hue onto the valid side of the boundary.
            const float reliable_chroma_blend = smoothstep(0.01F, 0.30F, local_chroma_risk);
            target_chroma_r = std::lerp(target_chroma_r, reliable_chroma_r, reliable_chroma_blend);
            target_chroma_g = std::lerp(target_chroma_g, reliable_chroma_g, reliable_chroma_blend);
            target_chroma_b = std::lerp(target_chroma_b, reliable_chroma_b, reliable_chroma_blend);
            // The sensor-risk classifier already excludes a one-channel saturated emitter. Once
            // it reports multi-channel loss, do not add a second scene-RGB disagreement gate: a
            // camera matrix can make a small camera-space ratio error visibly magenta while this
            // normalized RGB distance still looks small. This repair is itself evaluated in
            // camera RGB, so equal components express the sensor-neutral axis rather than display
            // grey. Risk-only boundary pixels retain the local illuminant. The physical shared
            // core loses its measured ratio, so it keeps following the
            // segment's boundary chroma instead of inventing a second transition toward display
            // grey.
            // The physical boundary's continuous influence bell adopts reliable neighbouring
            // luminance, while the unknowable deep core adopts only the broader locally observed
            // light surface. This removes the 99/100 platform edge and quantised core plateaus
            // without importing a distant guide colour or touching the far measured exterior.
            const float bounded_influence = std::clamp(local_influence, 0.0F, 1.0F);
            const float boundary_luminance_weight =
                std::sqrt(std::max(0.0F, 4.0F * bounded_influence * (1.0F - bounded_influence)));
            const float reliable_boundary_luminance =
                std::lerp(observed_smooth_luminance, guide_luminance, 0.82F);
            const float core_luminance_blend = 0.92F * smoothstep(0.48F, 0.90F, bounded_influence);
            const float shouldered_core_luminance = scene_referred_terminal_shoulder(
                observed_smooth_luminance,
                reliable_boundary_luminance
            );
            // Keep the luminance transition monotonic inside factual shared clipping. The earlier
            // symmetric boundary bell first pulled the shoulder toward the guide and then raised
            // the deep core again, so a strong highlight pull exposed its trough as a second arc.
            // Only the shared clipped core owns luminance reconstruction; all measured exterior
            // pixels were excluded from this pass before guide sampling.
            // Source reconstruction may restore missing energy but must never pre-darken a
            // terminal sample. Highlight/white controls own the later compression. Lowering a
            // subset of the shared core here exposes the mask topology as a second arc when those
            // controls are pulled hard, and makes the result darker than a scene-referred
            // reconstruction that retains its clipped plateau.
            const float target_luminance = std::max(input_luminance, shouldered_core_luminance);
            const auto target_scene_unit = transform_rgb(
                matrices.camera_to_scene,
                target_chroma_r,
                target_chroma_g,
                target_chroma_b
            );
            const float target_unit_luminance = positive_luminance(
                target_scene_unit[0U],
                target_scene_unit[1U],
                target_scene_unit[2U]
            );
            const float target_scale =
                target_luminance / std::max(target_unit_luminance, minimum_luminance);
            const std::array<float, 3U> target{
                target_scene_unit[0U] * target_scale,
                target_scene_unit[1U] * target_scale,
                target_scene_unit[2U] * target_scale,
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
                smoothstep(0.01F, 0.45F, local_chroma_risk) * chroma_repair_brightness_gate;
            // Multi-scale guides estimate the replacement surface, never its ownership. Keep the
            // final write within full-resolution factual clipping. This mirrors darktable's useful
            // separation between a dilated neighbourhood used to estimate colour and an undilated
            // damaged-site mask used for writes, and stops guide resampling from growing a yellow
            // halo around a lamp or solar disc.
            const float clipped_boundary_blend =
                clipped ? boundary_blend * observed_edge_gate : 0.0F;
            const float spatial_blend = std::max(clipped_boundary_blend, chroma_repair_blend);
            // Projection bins can straddle a clipped lamp and a dark fixture. Even when such a
            // pixel carries factual clipping and coherent mask support, its repaired RGB remains
            // strong ownership evidence for the dark edge. Dense bright cores pass the looser
            // clipped threshold; an exterior shoulder still uses the stricter measured-light
            // threshold.
            const float brightness_ratio =
                input_luminance / std::max(guide_luminance, minimum_luminance);
            const float brightness_gate = smoothstep(0.50F, 0.92F, brightness_ratio);
            // Candidate colour may come from a wider trustworthy neighbourhood, but the write is
            // owned only by the exact fraction of this output bin whose red, green, and blue CFA
            // contributions are all physically terminal. Unlike chroma risk, this coverage stays
            // zero for a lone early-clipping channel and cannot grow a lamp boundary into a line.
            const float write_ownership = shared_clipped_coverage;
            const float blend = spatial_blend * brightness_gate * write_ownership;
            if (blend <= 0.0F) {
                continue;
            }
            float output_r = std::lerp(input_r, target[0U], blend);
            float output_g = std::lerp(input_g, target[1U], blend);
            float output_b = std::lerp(input_b, target[2U], blend);
            // Chroma-risk topology must not become a second luminance topology. Reproject the
            // blended colour onto the exact interpolated luminance so differently quantised risk
            // bands remain brightness-identical when the physical clipping influence is equal.
            const float luminance_blend =
                core_luminance_blend * observed_edge_gate * brightness_gate * write_ownership;
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
    // RAW source preparation has now consumed this sidecar's unmeasured-CFA-colour evidence:
    // the demosaic path performs the continuous camera-space repair and this surface pass repairs
    // its remaining low-frequency topology. Leaving the original map behind would make a strong
    // later highlight pull desaturate the same footprint a second time, revealing the sensor-risk
    // boundary as a grey ring. Keep the factual clipping mask for diagnostics, but publish zero
    // residual chroma risk to the grade path.
    complete_cfa_owned_highlight_reconstruction(highlight_chroma_risk);
    return stats;
}

} // namespace shadow::image::raw_pipeline_detail
