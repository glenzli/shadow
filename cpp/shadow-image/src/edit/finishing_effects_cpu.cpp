#include "finishing_effects_cpu.hpp"

#include "adjustment_node_diagnostics.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace shadow::image::detail {

namespace {

constexpr std::size_t rgb_channels = 3U;

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

[[nodiscard]] double coordinate_noise(const std::uint32_t x, const std::uint32_t y,
                                      const std::uint32_t seed) noexcept {
    std::uint32_t value = x * 0x9e3779b9U ^ y * 0x85ebca6bU ^ seed;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<double>(value) /
               static_cast<double>(std::numeric_limits<std::uint32_t>::max()) * 2.0 -
           1.0;
}

} // namespace

void apply_finishing_effects_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                 const std::size_t node_index, const SharpenAdjustment& parameters,
                                 const AdjustmentExecutionContext& context) {
    if (parameters.grain_amount == 0.0 && parameters.vignette_amount == 0.0) {
        return;
    }

    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const auto weights = image.working_space.luminance_coefficients;
    const double full_width = context.full_dimensions.width;
    const double full_height = context.full_dimensions.height;
    const std::uint32_t grain_block =
        1U + static_cast<std::uint32_t>(std::round(parameters.grain_size * 3.0));

    for (std::uint32_t y = 0; y < image.dimensions.height; ++y) {
        for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
            const std::size_t sample =
                static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * rgb_channels;
            const std::uint32_t global_x = context.origin_x + x;
            const std::uint32_t global_y = context.origin_y + y;
            const double luma = image.samples[sample] * weights[0] +
                                image.samples[sample + 1U] * weights[1] +
                                image.samples[sample + 2U] * weights[2];
            double gain = 1.0;
            double additive = 0.0;
            if (parameters.grain_amount > 0.0) {
                const double coarse =
                    coordinate_noise(global_x / grain_block, global_y / grain_block, 0x51ed270bU);
                const double fine = coordinate_noise(global_x, global_y, 0xa54ff53aU);
                const double noise = std::lerp(coarse, fine, parameters.grain_roughness);
                const double visibility = 0.45 + 0.55 * (1.0 - smooth_transition(0.0, 1.0, luma));
                additive = noise * parameters.grain_amount *
                           (0.012 + 0.035 * parameters.grain_roughness) * visibility;
            }
            if (parameters.vignette_amount != 0.0) {
                double nx = (static_cast<double>(global_x) + 0.5) / full_width * 2.0 - 1.0;
                double ny = (static_cast<double>(global_y) + 0.5) / full_height * 2.0 - 1.0;
                nx *= full_width / std::max(full_width, full_height);
                ny *= full_height / std::max(full_width, full_height);
                const double circle = std::hypot(nx, ny);
                const double square = std::max(std::abs(nx), std::abs(ny));
                const double round_mix = 0.5 * (parameters.vignette_roundness + 1.0);
                const double radius = std::lerp(square, circle, round_mix);
                const double start = 0.15 + 0.65 * parameters.vignette_midpoint;
                const double feather = 0.04 + 0.50 * parameters.vignette_feather;
                const double mask = smooth_transition(start, start + feather, radius);
                double stops = 2.0 * parameters.vignette_amount * mask;
                if (stops < 0.0) {
                    const double highlight = smooth_transition(0.6, 1.6, luma);
                    stops *= 1.0 - parameters.vignette_highlights * highlight;
                }
                gain = std::exp2(stops);
            }
            for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                image.samples[sample + channel] = checked_edit_pixel_float(
                    static_cast<double>(image.samples[sample + channel]) * gain + additive,
                    node_index, node);
            }
        }
    }
}

} // namespace shadow::image::detail
