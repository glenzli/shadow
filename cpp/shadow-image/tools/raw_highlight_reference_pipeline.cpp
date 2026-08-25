#include "raw_highlight_reference_pipeline.hpp"

#include "../src/raw/bayer_sampling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace shadow::image::probe_detail {
namespace {

// The reference maths and mask topology below are adapted from darktable's GPLv3
// src/iop/hlreconstruct/opposed.c at 943d74a50e5baeecee26005cf20309e32f487949. Shadow is GPLv3;
// retaining the upstream identity here makes later comparisons auditable instead of folklore.

constexpr float darktable_opposed_clip_magic = 0.987F;
constexpr std::uint32_t mask_block_extent = 3U;

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

[[nodiscard]] RawCfaColor active_cfa_color_at(
    const DarktableOpposedReferencePlane& plane,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    return plane.bayer_2x2[(y & 1U) * 2U + (x & 1U)];
}

[[nodiscard]] std::size_t
plane_index(const Dimensions dimensions, const std::uint32_t x, const std::uint32_t y) noexcept {
    return static_cast<std::size_t>(y) * dimensions.width + x;
}

[[nodiscard]] float opposed_reference_at(
    const std::vector<float>& input,
    const DarktableOpposedReferencePlane& plane,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    const std::uint32_t first_x = x == 0U ? 0U : x - 1U;
    const std::uint32_t first_y = y == 0U ? 0U : y - 1U;
    // These exclusive upper bounds intentionally mirror darktable's _calc_refavg rather than
    // silently substituting Shadow's same-colour interpolation at the frame border.
    const std::uint32_t last_x = std::min(plane.dimensions.width - 1U, x + 2U);
    const std::uint32_t last_y = std::min(plane.dimensions.height - 1U, y + 2U);
    for (std::uint32_t sample_y = first_y; sample_y < last_y; ++sample_y) {
        for (std::uint32_t sample_x = first_x; sample_x < last_x; ++sample_x) {
            const int channel = rgb_channel(active_cfa_color_at(plane, sample_x, sample_y));
            if (channel < 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(channel);
            totals[index] +=
                std::max(0.0F, input[plane_index(plane.dimensions, sample_x, sample_y)]);
            ++counts[index];
        }
    }
    std::array<float, 3U> cube_roots{};
    for (std::size_t channel = 0U; channel < cube_roots.size(); ++channel) {
        if (counts[channel] != 0U) {
            cube_roots[channel] = std::cbrt(
                static_cast<float>(totals[channel] / static_cast<double>(counts[channel]))
            );
        }
    }
    const int current_channel = rgb_channel(active_cfa_color_at(plane, x, y));
    if (current_channel < 0) {
        return 0.0F;
    }
    const auto channel = static_cast<std::size_t>(current_channel);
    const std::array<float, 3U> opposed_cube_roots{
        0.5F * (cube_roots[1U] + cube_roots[2U]),
        0.5F * (cube_roots[0U] + cube_roots[2U]),
        0.5F * (cube_roots[0U] + cube_roots[1U]),
    };
    return opposed_cube_roots[channel] * opposed_cube_roots[channel] * opposed_cube_roots[channel];
}

[[nodiscard]] bool
dilated_mask_value(const std::uint8_t* const input, const std::size_t width) noexcept {
    if (input[0] != 0U) {
        return true;
    }
    if ((input[-static_cast<std::ptrdiff_t>(width) - 1] != 0U)
        || (input[-static_cast<std::ptrdiff_t>(width)] != 0U)
        || (input[-static_cast<std::ptrdiff_t>(width) + 1] != 0U) || (input[-1] != 0U)
        || (input[1] != 0U) || (input[static_cast<std::ptrdiff_t>(width) - 1] != 0U)
        || (input[static_cast<std::ptrdiff_t>(width)] != 0U)
        || (input[static_cast<std::ptrdiff_t>(width) + 1] != 0U)) {
        return true;
    }
    constexpr std::array<std::array<int, 2U>, 36U> outer_offsets{{
        {{-2, -3}}, {{-1, -3}}, {{0, -3}}, {{1, -3}}, {{2, -3}}, {{-3, -2}},
        {{-2, -2}}, {{-1, -2}}, {{0, -2}}, {{1, -2}}, {{2, -2}}, {{3, -2}},
        {{-3, -1}}, {{-2, -1}}, {{2, -1}}, {{3, -1}}, {{-3, 0}}, {{-2, 0}},
        {{2, 0}},   {{3, 0}},   {{-3, 1}}, {{-2, 1}}, {{2, 1}},  {{3, 1}},
        {{-3, 2}},  {{-2, 2}},  {{-1, 2}}, {{0, 2}},  {{1, 2}},  {{2, 2}},
        {{3, 2}},   {{-2, 3}},  {{-1, 3}}, {{0, 3}},  {{1, 3}},  {{2, 3}},
    }};
    for (const auto [dx, dy] : outer_offsets) {
        const auto offset = static_cast<std::ptrdiff_t>(dy) * static_cast<std::ptrdiff_t>(width)
                            + static_cast<std::ptrdiff_t>(dx);
        if (input[offset] != 0U) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::array<float, 3U> effective_white_balance_gains_for(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const detail::BayerCfaSamplingPolicy policy
) noexcept {
    std::array<double, 3U> gains{};
    std::array<std::uint32_t, 3U> counts{};
    for (std::size_t site = 0U; site < frame.descriptor.bayer_2x2.size(); ++site) {
        const int channel = rgb_channel(frame.descriptor.bayer_2x2[site]);
        if (channel < 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(channel);
        gains[index] += transform.apply_cfa_white_balance ? transform.cfa_white_balance[site] : 1.0;
        ++counts[index];
    }
    std::array<float, 3U> effective_gains{};
    for (std::size_t channel = 0U; channel < effective_gains.size(); ++channel) {
        const double gain = counts[channel] == 0U ? 1.0 : gains[channel] / counts[channel];
        effective_gains[channel] = static_cast<float>(gain) * policy.white_balance_scale;
    }
    return effective_gains;
}

[[nodiscard]] std::array<float, 3U> missing_channel_fallback(
    const DarktableOpposedReferencePlane& plane,
    const std::uint32_t center_x,
    const std::uint32_t center_y
) noexcept {
    std::array<float, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    for (std::uint32_t radius = 0U; radius <= 3U; ++radius) {
        const std::uint32_t first_x = center_x > radius ? center_x - radius : 0U;
        const std::uint32_t first_y = center_y > radius ? center_y - radius : 0U;
        const std::uint32_t last_x = std::min(plane.dimensions.width, center_x + radius + 1U);
        const std::uint32_t last_y = std::min(plane.dimensions.height, center_y + radius + 1U);
        for (std::uint32_t y = first_y; y < last_y; ++y) {
            for (std::uint32_t x = first_x; x < last_x; ++x) {
                const int channel = rgb_channel(active_cfa_color_at(plane, x, y));
                if (channel < 0) {
                    continue;
                }
                const auto index = static_cast<std::size_t>(channel);
                totals[index] += plane.reconstructed_samples[plane_index(plane.dimensions, x, y)];
                ++counts[index];
            }
        }
        if (std::all_of(counts.begin(), counts.end(), [](const auto count) {
                return count != 0U;
            })) {
            break;
        }
    }
    std::array<float, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        result[channel] =
            counts[channel] == 0U ? 0.0F : totals[channel] / static_cast<float>(counts[channel]);
    }
    return result;
}

} // namespace

bool DarktableOpposedReferencePlane::valid() const noexcept {
    const auto count = dimensions.pixel_count();
    return dimensions.width != 0U && dimensions.height != 0U
           && count <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
           && measured_samples.size() == static_cast<std::size_t>(count)
           && reconstructed_samples.size() == measured_samples.size();
}

DarktableOpposedReferencePlane make_darktable_opposed_reference_plane(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform
) {
    detail::validate_bayer_frame(frame, "darktable opposed reference");
    auto policy = detail::editable_raw_cfa_sampling_policy(transform);
    policy.require_shared_terminal_headroom = false;
    policy.reconstruct_terminal_highlights = false;
    policy.feather_highlight_chroma_neutralization = false;

    DarktableOpposedReferencePlane plane;
    plane.dimensions = frame.descriptor.active_dimensions;
    const std::uint32_t first_raw_x = frame.descriptor.active_margins.left;
    const std::uint32_t first_raw_y = frame.descriptor.active_margins.top;
    for (std::uint32_t phase_y = 0U; phase_y < 2U; ++phase_y) {
        for (std::uint32_t phase_x = 0U; phase_x < 2U; ++phase_x) {
            const std::uint32_t raw_x = first_raw_x + phase_x;
            const std::uint32_t raw_y = first_raw_y + phase_y;
            plane.bayer_2x2[phase_y * 2U + phase_x] =
                frame.descriptor.bayer_2x2[(raw_y & 1U) * 2U + (raw_x & 1U)];
        }
    }
    const auto sample_count = static_cast<std::size_t>(plane.dimensions.pixel_count());
    plane.measured_samples.resize(sample_count);
    plane.reconstructed_samples.resize(sample_count);
    plane.effective_white_balance_gains =
        effective_white_balance_gains_for(frame, transform, policy);
    for (std::size_t channel = 0U; channel < plane.clip_values.size(); ++channel) {
        plane.clip_values[channel] =
            darktable_opposed_clip_magic * plane.effective_white_balance_gains[channel];
    }

    for (std::uint32_t y = 0U; y < plane.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < plane.dimensions.width; ++x) {
            const auto sample = detail::opposed_highlight_cfa_sample_at(
                frame,
                first_raw_x + x,
                first_raw_y + y,
                &transform,
                policy
            );
            const auto index = plane_index(plane.dimensions, x, y);
            plane.measured_samples[index] = sample.measured;
            plane.reconstructed_samples[index] = sample.measured;
        }
    }

    const std::uint32_t mask_width = plane.dimensions.width / mask_block_extent;
    const std::uint32_t mask_height = plane.dimensions.height / mask_block_extent;
    if (mask_width == 0U || mask_height == 0U) {
        return plane;
    }
    const std::size_t mask_size = static_cast<std::size_t>(mask_width) * mask_height;
    std::vector<std::uint8_t> masks(mask_size * 6U);
    bool any_clipped = false;
    if (mask_width > 1U && mask_height > 1U) {
        for (std::uint32_t block_y = 0U; block_y + 1U < mask_height; ++block_y) {
            for (std::uint32_t block_x = 0U; block_x + 1U < mask_width; ++block_x) {
                std::array<bool, 3U> block_clipped{};
                for (std::uint32_t local_y = 0U; local_y < mask_block_extent; ++local_y) {
                    for (std::uint32_t local_x = 0U; local_x < mask_block_extent; ++local_x) {
                        const std::uint32_t x = block_x * mask_block_extent + local_x;
                        const std::uint32_t y = block_y * mask_block_extent + local_y;
                        const int channel = rgb_channel(active_cfa_color_at(plane, x, y));
                        if (channel < 0) {
                            continue;
                        }
                        const auto channel_index = static_cast<std::size_t>(channel);
                        block_clipped[channel_index] =
                            block_clipped[channel_index]
                            || plane.measured_samples[plane_index(plane.dimensions, x, y)]
                                   >= plane.clip_values[channel_index];
                    }
                }
                const auto mask_index = static_cast<std::size_t>(block_y) * mask_width + block_x;
                for (std::size_t channel = 0U; channel < block_clipped.size(); ++channel) {
                    masks[channel * mask_size + mask_index] = block_clipped[channel] ? 1U : 0U;
                    any_clipped = any_clipped || block_clipped[channel];
                }
            }
        }
    }

    if (any_clipped) {
        for (std::uint32_t block_y = 0U; block_y < mask_height; ++block_y) {
            for (std::uint32_t block_x = 0U; block_x < mask_width; ++block_x) {
                const auto mask_index = static_cast<std::size_t>(block_y) * mask_width + block_x;
                const bool safe = block_x >= 3U && block_y >= 3U && block_x + 4U < mask_width
                                  && block_y + 4U < mask_height;
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    const auto* source = masks.data() + channel * mask_size + mask_index;
                    masks[(channel + 3U) * mask_size + mask_index] =
                        safe && dilated_mask_value(source, mask_width)
                            ? 1U
                            : masks[channel * mask_size + mask_index];
                }
            }
        }

        std::array<long double, 3U> sums{};
        for (std::uint32_t y = 0U; y < plane.dimensions.height; ++y) {
            for (std::uint32_t x = 0U; x < plane.dimensions.width; ++x) {
                const int channel = rgb_channel(active_cfa_color_at(plane, x, y));
                if (channel < 0) {
                    continue;
                }
                const auto channel_index = static_cast<std::size_t>(channel);
                const float measured = plane.measured_samples[plane_index(plane.dimensions, x, y)];
                if (measured >= plane.clip_values[channel_index]
                    || measured <= 0.2F * plane.clip_values[channel_index]) {
                    continue;
                }
                const std::uint32_t block_x = std::min(mask_width - 1U, x / mask_block_extent);
                const std::uint32_t block_y = std::min(mask_height - 1U, y / mask_block_extent);
                const auto mask_index = static_cast<std::size_t>(block_y) * mask_width + block_x;
                if (masks[(channel_index + 3U) * mask_size + mask_index] == 0U) {
                    continue;
                }
                sums[channel_index] += static_cast<long double>(
                    measured - opposed_reference_at(plane.measured_samples, plane, x, y)
                );
                ++plane.chrominance_support[channel_index];
            }
        }
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            if (plane.chrominance_support[channel] > 100U) {
                plane.chrominance_offsets[channel] = static_cast<float>(
                    sums[channel] / static_cast<long double>(plane.chrominance_support[channel])
                );
            }
        }
    }

    for (std::uint32_t y = 0U; y < plane.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < plane.dimensions.width; ++x) {
            const int channel = rgb_channel(active_cfa_color_at(plane, x, y));
            if (channel < 0) {
                continue;
            }
            const auto channel_index = static_cast<std::size_t>(channel);
            const auto index = plane_index(plane.dimensions, x, y);
            const float measured = plane.measured_samples[index];
            if (measured < plane.clip_values[channel_index]) {
                continue;
            }
            ++plane.clipped_photosites;
            ++plane.clipped_photosites_by_channel[channel_index];
            const float reference = opposed_reference_at(plane.measured_samples, plane, x, y)
                                    + plane.chrominance_offsets[channel_index];
            plane.reconstructed_samples[index] = std::max(measured, reference);
            if (plane.reconstructed_samples[index] > measured) {
                ++plane.changed_photosites;
                ++plane.changed_photosites_by_channel[channel_index];
            }
        }
    }
    return plane;
}

std::array<float, 3U> darktable_opposed_area_camera_rgb_at(
    const DarktableOpposedReferencePlane& plane,
    const Dimensions target_dimensions,
    const std::uint32_t target_x,
    const std::uint32_t target_y
) {
    if (!plane.valid() || target_dimensions.width == 0U || target_dimensions.height == 0U
        || target_dimensions.width > plane.dimensions.width
        || target_dimensions.height > plane.dimensions.height || target_x >= target_dimensions.width
        || target_y >= target_dimensions.height) {
        throw std::invalid_argument("invalid darktable opposed reference area sample request");
    }
    const double scale_x = static_cast<double>(plane.dimensions.width) / target_dimensions.width;
    const double scale_y = static_cast<double>(plane.dimensions.height) / target_dimensions.height;
    const double source_left = static_cast<double>(target_x) * scale_x;
    const double source_right = static_cast<double>(target_x + 1U) * scale_x;
    const double source_top = static_cast<double>(target_y) * scale_y;
    const double source_bottom = static_cast<double>(target_y + 1U) * scale_y;
    const auto first_x = static_cast<std::uint32_t>(std::floor(source_left));
    const auto first_y = static_cast<std::uint32_t>(std::floor(source_top));
    const auto last_x =
        std::min(plane.dimensions.width, static_cast<std::uint32_t>(std::ceil(source_right)));
    const auto last_y =
        std::min(plane.dimensions.height, static_cast<std::uint32_t>(std::ceil(source_bottom)));
    std::array<double, 3U> totals{};
    std::array<double, 3U> weights{};
    for (std::uint32_t y = first_y; y < last_y; ++y) {
        const double overlap_y = std::max(
            0.0,
            std::min(source_bottom, static_cast<double>(y + 1U))
                - std::max(source_top, static_cast<double>(y))
        );
        for (std::uint32_t x = first_x; x < last_x; ++x) {
            const int channel = rgb_channel(active_cfa_color_at(plane, x, y));
            if (channel < 0) {
                continue;
            }
            const double overlap_x = std::max(
                0.0,
                std::min(source_right, static_cast<double>(x + 1U))
                    - std::max(source_left, static_cast<double>(x))
            );
            const auto channel_index = static_cast<std::size_t>(channel);
            const double weight = overlap_x * overlap_y;
            totals[channel_index] +=
                plane.reconstructed_samples[plane_index(plane.dimensions, x, y)] * weight;
            weights[channel_index] += weight;
        }
    }
    const auto center_x = std::min(
        plane.dimensions.width - 1U,
        static_cast<std::uint32_t>((source_left + source_right) * 0.5)
    );
    const auto center_y = std::min(
        plane.dimensions.height - 1U,
        static_cast<std::uint32_t>((source_top + source_bottom) * 0.5)
    );
    const auto fallback = missing_channel_fallback(plane, center_x, center_y);
    std::array<float, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        result[channel] = weights[channel] <= 0.0
                              ? fallback[channel]
                              : static_cast<float>(totals[channel] / weights[channel]);
    }
    return result;
}

} // namespace shadow::image::probe_detail
