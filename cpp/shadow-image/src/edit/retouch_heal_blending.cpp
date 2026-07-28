#include "retouch_heal_blending.hpp"

#include <shadow/image/edit_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shadow::image::detail {

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr float minimum_coverage = 1.0e-4F;
constexpr std::size_t poisson_iterations = 28U;
constexpr double screening_weight = 4.0;

[[noreturn]] void invalid_heal(const std::string& detail) {
    throw EditError(EditErrorCode::invalid_parameter, std::nullopt,
                    "spot-heal texture blending " + detail);
}

[[nodiscard]] std::size_t sample_index(const FloatRgbImage& image, const std::uint32_t x,
                                       const std::uint32_t y) {
    return static_cast<std::size_t>(y) * (image.row_stride_bytes / sizeof(float)) +
           static_cast<std::size_t>(x) * rgb_channels;
}

[[nodiscard]] std::array<float, rgb_channels> sample_bilinear(const FloatRgbImage& image,
                                                              const double x, const double y) {
    const double clamped_x = std::clamp(x, 0.0, static_cast<double>(image.dimensions.width - 1U));
    const double clamped_y = std::clamp(y, 0.0, static_cast<double>(image.dimensions.height - 1U));
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

    std::array<float, rgb_channels> result{};
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        const double top =
            std::lerp(static_cast<double>(image.samples[top_left + channel]),
                      static_cast<double>(image.samples[top_right + channel]), blend_x);
        const double bottom =
            std::lerp(static_cast<double>(image.samples[bottom_left + channel]),
                      static_cast<double>(image.samples[bottom_right + channel]), blend_x);
        result[channel] = static_cast<float>(std::lerp(top, bottom, blend_y));
    }
    return result;
}

[[nodiscard]] std::size_t local_index(const std::uint32_t x, const std::uint32_t y,
                                      const std::uint32_t width) {
    return static_cast<std::size_t>(y) * width + x;
}

[[nodiscard]] float robust_median(std::vector<float>& values) {
    if (values.empty()) {
        return 0.0F;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), middle, values.end());
    const float upper = *middle;
    if (values.size() % 2U != 0U) {
        return upper;
    }
    const float lower = *std::max_element(values.begin(), middle);
    return std::midpoint(lower, upper);
}

[[nodiscard]] bool covered(const std::span<const float> coverage, const std::uint32_t width,
                           const std::uint32_t x, const std::uint32_t y) {
    return coverage[local_index(x, y, width)] > minimum_coverage;
}

} // namespace

void apply_texture_heal(FloatRgbImage& destination, const FloatRgbImage& source,
                        const std::span<const float> coverage, const std::int64_t coverage_origin_x,
                        const std::int64_t coverage_origin_y, const std::uint32_t coverage_width,
                        const std::uint32_t coverage_height, const double source_offset_x_pixels,
                        const double source_offset_y_pixels) {
    const std::uint64_t coverage_pixels =
        static_cast<std::uint64_t>(coverage_width) * coverage_height;
    if (destination.dimensions != source.dimensions ||
        destination.samples.size() != source.samples.size() || coverage_width == 0U ||
        coverage_height == 0U || coverage_pixels != coverage.size() || coverage_origin_x < 0 ||
        coverage_origin_y < 0 ||
        static_cast<std::uint64_t>(coverage_origin_x) + coverage_width >
            destination.dimensions.width ||
        static_cast<std::uint64_t>(coverage_origin_y) + coverage_height >
            destination.dimensions.height ||
        !std::isfinite(source_offset_x_pixels) || !std::isfinite(source_offset_y_pixels)) {
        invalid_heal("received an invalid source or coverage layout");
    }

    const std::size_t pixel_count = static_cast<std::size_t>(coverage_pixels);
    std::vector<float> donor(pixel_count * rgb_channels, 0.0F);
    std::vector<float> solution(pixel_count * rgb_channels, 0.0F);
    std::array<std::vector<float>, rgb_channels> target_boundary_samples;
    std::array<std::vector<float>, rgb_channels> donor_region_samples;
    for (auto& samples : target_boundary_samples) {
        samples.reserve(static_cast<std::size_t>(coverage_width + coverage_height) * 2U);
    }
    for (auto& samples : donor_region_samples) {
        samples.reserve(pixel_count);
    }

    for (std::uint32_t local_y = 0U; local_y < coverage_height; ++local_y) {
        for (std::uint32_t local_x = 0U; local_x < coverage_width; ++local_x) {
            const auto raster_x = static_cast<std::uint32_t>(coverage_origin_x + local_x);
            const auto raster_y = static_cast<std::uint32_t>(coverage_origin_y + local_y);
            const std::size_t pixel = local_index(local_x, local_y, coverage_width);
            const auto donor_sample =
                sample_bilinear(source, static_cast<double>(raster_x) + source_offset_x_pixels,
                                static_cast<double>(raster_y) + source_offset_y_pixels);
            const std::size_t raster_sample = sample_index(source, raster_x, raster_y);
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                donor[pixel * rgb_channels + channel] = donor_sample[channel];
                solution[pixel * rgb_channels + channel] = donor_sample[channel];
            }

            if (covered(coverage, coverage_width, local_x, local_y)) {
                for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                    donor_region_samples[channel].push_back(donor_sample[channel]);
                }
                continue;
            }
            const bool boundary =
                (local_x > 0U && covered(coverage, coverage_width, local_x - 1U, local_y)) ||
                (local_x + 1U < coverage_width &&
                 covered(coverage, coverage_width, local_x + 1U, local_y)) ||
                (local_y > 0U && covered(coverage, coverage_width, local_x, local_y - 1U)) ||
                (local_y + 1U < coverage_height &&
                 covered(coverage, coverage_width, local_x, local_y + 1U));
            if (!boundary) {
                continue;
            }
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                target_boundary_samples[channel].push_back(source.samples[raster_sample + channel]);
            }
        }
    }

    if (target_boundary_samples[0].empty() || donor_region_samples[0].empty()) {
        return;
    }
    std::array<float, rgb_channels> boundary_shift{};
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        boundary_shift[channel] = robust_median(target_boundary_samples[channel]) -
                                  robust_median(donor_region_samples[channel]);
    }
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
            solution[pixel * rgb_channels + channel] += boundary_shift[channel];
        }
    }
    const std::vector<float> screened_target = solution;

    constexpr std::array<std::array<std::int32_t, 2U>, 4U> neighbors{{
        {{-1, 0}},
        {{1, 0}},
        {{0, -1}},
        {{0, 1}},
    }};
    for (std::size_t iteration = 0U; iteration < poisson_iterations; ++iteration) {
        for (std::uint32_t local_y = 0U; local_y < coverage_height; ++local_y) {
            for (std::uint32_t local_x = 0U; local_x < coverage_width; ++local_x) {
                if (!covered(coverage, coverage_width, local_x, local_y)) {
                    continue;
                }
                const std::size_t pixel = local_index(local_x, local_y, coverage_width);
                for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                    double neighbor_sum = 0.0;
                    double donor_laplacian = 0.0;
                    std::uint32_t neighbor_count = 0U;
                    for (const auto neighbor : neighbors) {
                        const std::int64_t neighbor_x =
                            static_cast<std::int64_t>(local_x) + neighbor[0];
                        const std::int64_t neighbor_y =
                            static_cast<std::int64_t>(local_y) + neighbor[1];
                        if (neighbor_x < 0 || neighbor_y < 0 ||
                            neighbor_x >= static_cast<std::int64_t>(coverage_width) ||
                            neighbor_y >= static_cast<std::int64_t>(coverage_height)) {
                            continue;
                        }
                        const auto adjacent_x = static_cast<std::uint32_t>(neighbor_x);
                        const auto adjacent_y = static_cast<std::uint32_t>(neighbor_y);
                        const std::size_t adjacent =
                            local_index(adjacent_x, adjacent_y, coverage_width);
                        const bool adjacent_covered =
                            covered(coverage, coverage_width, adjacent_x, adjacent_y);
                        if (adjacent_covered) {
                            neighbor_sum += solution[adjacent * rgb_channels + channel];
                        } else {
                            const auto adjacent_raster_x =
                                static_cast<std::uint32_t>(coverage_origin_x + adjacent_x);
                            const auto adjacent_raster_y =
                                static_cast<std::uint32_t>(coverage_origin_y + adjacent_y);
                            neighbor_sum += source.samples[sample_index(source, adjacent_raster_x,
                                                                        adjacent_raster_y) +
                                                           channel];
                        }
                        donor_laplacian +=
                            static_cast<double>(donor[pixel * rgb_channels + channel]) -
                            static_cast<double>(donor[adjacent * rgb_channels + channel]);
                        ++neighbor_count;
                    }
                    if (neighbor_count == 0U) {
                        continue;
                    }
                    const double value =
                        (neighbor_sum + donor_laplacian +
                         screening_weight *
                             static_cast<double>(screened_target[pixel * rgb_channels + channel])) /
                        (static_cast<double>(neighbor_count) + screening_weight);
                    if (!std::isfinite(value) ||
                        value < static_cast<double>(std::numeric_limits<float>::lowest()) ||
                        value > static_cast<double>(std::numeric_limits<float>::max())) {
                        invalid_heal("produced a non-finite gradient-domain sample");
                    }
                    solution[pixel * rgb_channels + channel] = static_cast<float>(value);
                }
            }
        }
    }

    for (std::uint32_t local_y = 0U; local_y < coverage_height; ++local_y) {
        for (std::uint32_t local_x = 0U; local_x < coverage_width; ++local_x) {
            const std::size_t pixel = local_index(local_x, local_y, coverage_width);
            const double alpha = std::clamp(static_cast<double>(coverage[pixel]), 0.0, 1.0);
            if (alpha <= 0.0) {
                continue;
            }
            const auto raster_x = static_cast<std::uint32_t>(coverage_origin_x + local_x);
            const auto raster_y = static_cast<std::uint32_t>(coverage_origin_y + local_y);
            const std::size_t raster_sample = sample_index(destination, raster_x, raster_y);
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                destination.samples[raster_sample + channel] = static_cast<float>(std::lerp(
                    static_cast<double>(source.samples[raster_sample + channel]),
                    static_cast<double>(solution[pixel * rgb_channels + channel]), alpha));
            }
        }
    }
}

} // namespace shadow::image::detail
