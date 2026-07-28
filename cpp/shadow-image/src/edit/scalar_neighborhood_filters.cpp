#include "scalar_neighborhood_filters.hpp"

#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

constexpr std::uint32_t minimum_rows_per_chunk = 32U;

template <typename Work> void parallel_for_scalar_rows(const std::size_t extent, Work&& work) {
    if (extent > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("scalar field extent exceeds the row scheduler contract");
    }
    parallel_for_rows(static_cast<std::uint32_t>(extent), minimum_rows_per_chunk,
                      std::forward<Work>(work));
}

[[nodiscard]] std::size_t checked_field_size(const std::size_t width, const std::size_t height) {
    if (width == 0U || height == 0U) {
        throw std::invalid_argument("scalar field dimensions must be non-zero");
    }
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::overflow_error("scalar field dimensions exceed the address space");
    }
    return width * height;
}

template <typename Value>
void validate_field_shape(const std::span<const Value> source, const std::size_t width,
                          const std::size_t height) {
    if (source.size() != checked_field_size(width, height)) {
        throw std::invalid_argument("scalar field storage does not match its dimensions");
    }
}

void validate_guided_filter_inputs(const std::span<const float> guide,
                                   const PreparedGuidedFilter& prepared, const double epsilon) {
    validate_field_shape(guide, prepared.width(), prepared.height());
    if (!std::isfinite(epsilon) || epsilon <= 0.0) {
        throw std::invalid_argument("guided-filter epsilon must be finite and positive");
    }
}

[[nodiscard]] std::vector<float> box_mean_scalar(const std::span<const float> source,
                                                 const std::size_t width, const std::size_t height,
                                                 const std::uint32_t radius) {
    validate_field_shape(source, width, height);
    if (radius == 0U) {
        return {source.begin(), source.end()};
    }

    std::vector<float> horizontal(source.size());
    std::vector<float> result(source.size());
    const auto clamped_index = [](const std::int64_t coordinate, const std::size_t extent) {
        return static_cast<std::size_t>(
            std::clamp(coordinate, std::int64_t{0}, static_cast<std::int64_t>(extent - 1U)));
    };
    const auto signed_radius = static_cast<std::int64_t>(radius);
    const double inverse_window = 1.0 / (2.0 * static_cast<double>(radius) + 1.0);

    parallel_for_scalar_rows(height, [&](const std::uint32_t first_row,
                                         const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = y * width;
            double sum = 0.0;
            for (std::int64_t offset = -signed_radius; offset <= signed_radius; ++offset) {
                sum += source[row + clamped_index(offset, width)];
            }
            horizontal[row] = static_cast<float>(sum * inverse_window);
            for (std::size_t x = 1U; x < width; ++x) {
                sum += source[row +
                              clamped_index(static_cast<std::int64_t>(x) + signed_radius, width)];
                sum -= source[row + clamped_index(static_cast<std::int64_t>(x) - signed_radius - 1,
                                                  width)];
                horizontal[row + x] = static_cast<float>(sum * inverse_window);
            }
        }
    });

    // Each column is independent in the second pass. Reuse the bounded row
    // scheduler with the x coordinate as its work index.
    parallel_for_scalar_rows(width, [&](const std::uint32_t first_column,
                                        const std::uint32_t past_last_column) {
        for (std::size_t x = first_column; x < past_last_column; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -signed_radius; offset <= signed_radius; ++offset) {
                sum += horizontal[clamped_index(offset, height) * width + x];
            }
            result[x] = static_cast<float>(sum * inverse_window);
            for (std::size_t y = 1U; y < height; ++y) {
                sum +=
                    horizontal[clamped_index(static_cast<std::int64_t>(y) + signed_radius, height) *
                                   width +
                               x];
                sum -= horizontal[clamped_index(static_cast<std::int64_t>(y) - signed_radius - 1,
                                                height) *
                                      width +
                                  x];
                result[y * width + x] = static_cast<float>(sum * inverse_window);
            }
        }
    });
    return result;
}

} // namespace

std::size_t reflect101_index(std::int64_t index, const std::size_t extent) noexcept {
    if (extent <= 1U) {
        return 0U;
    }
    const auto signed_extent = static_cast<std::int64_t>(extent);
    while (index < 0 || index >= signed_extent) {
        if (index < 0) {
            index = -index;
        } else {
            index = 2 * signed_extent - 2 - index;
        }
    }
    return static_cast<std::size_t>(index);
}

std::vector<double> gaussian_kernel(const double sigma, const std::uint32_t radius) {
    if (!std::isfinite(sigma) || sigma <= 0.0) {
        throw std::invalid_argument("Gaussian sigma must be finite and positive");
    }
    const std::size_t size = static_cast<std::size_t>(radius) * 2U + 1U;
    std::vector<double> kernel(size);
    const double inverse_two_sigma_squared = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (std::int64_t offset = -static_cast<std::int64_t>(radius);
         offset <= static_cast<std::int64_t>(radius); ++offset) {
        const double coordinate = static_cast<double>(offset);
        const double value = std::exp(-(coordinate * coordinate) * inverse_two_sigma_squared);
        kernel[static_cast<std::size_t>(offset + static_cast<std::int64_t>(radius))] = value;
        sum += value;
    }
    for (double& value : kernel) {
        value /= sum;
    }
    return kernel;
}

std::vector<double> gaussian_blur_scalar(const std::span<const double> source,
                                         const std::size_t width, const std::size_t height,
                                         const double sigma_x, const double sigma_y) {
    validate_field_shape(source, width, height);
    if (!std::isfinite(sigma_x) || sigma_x <= 0.0 || !std::isfinite(sigma_y) || sigma_y <= 0.0) {
        throw std::invalid_argument("Gaussian sigmas must be finite and positive");
    }

    const std::uint32_t radius_x =
        static_cast<std::uint32_t>(std::max(1.0, std::ceil(3.0 * sigma_x)));
    const std::uint32_t radius_y =
        static_cast<std::uint32_t>(std::max(1.0, std::ceil(3.0 * sigma_y)));
    const auto kernel_x = gaussian_kernel(std::max(0.20, sigma_x), radius_x);
    const auto kernel_y = gaussian_kernel(std::max(0.20, sigma_y), radius_y);
    std::vector<double> horizontal(source.size());
    std::vector<double> result(source.size());
    const auto signed_radius_x = static_cast<std::int64_t>(radius_x);
    const auto signed_radius_y = static_cast<std::int64_t>(radius_y);

    parallel_for_scalar_rows(height, [&](const std::uint32_t first_row,
                                         const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            for (std::size_t x = 0U; x < width; ++x) {
                double sum = 0.0;
                for (std::int64_t offset = -signed_radius_x; offset <= signed_radius_x; ++offset) {
                    const std::size_t source_x =
                        reflect101_index(static_cast<std::int64_t>(x) + offset, width);
                    sum += source[y * width + source_x] *
                           kernel_x[static_cast<std::size_t>(offset + signed_radius_x)];
                }
                horizontal[y * width + x] = sum;
            }
        }
    });
    parallel_for_scalar_rows(height, [&](const std::uint32_t first_row,
                                         const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            for (std::size_t x = 0U; x < width; ++x) {
                double sum = 0.0;
                for (std::int64_t offset = -signed_radius_y; offset <= signed_radius_y; ++offset) {
                    const std::size_t source_y =
                        reflect101_index(static_cast<std::int64_t>(y) + offset, height);
                    sum += horizontal[source_y * width + x] *
                           kernel_y[static_cast<std::size_t>(offset + signed_radius_y)];
                }
                result[y * width + x] = sum;
            }
        }
    });
    return result;
}

PreparedGuidedFilter::PreparedGuidedFilter(std::vector<float> mean, std::vector<float> variance,
                                           const std::size_t width, const std::size_t height,
                                           const std::uint32_t radius) noexcept
    : mean_(std::move(mean)), variance_(std::move(variance)), width_(width), height_(height),
      radius_(radius) {}

PreparedGuidedFilter prepare_replicated_guided_filter(const std::span<const float> guide,
                                                      const std::size_t width,
                                                      const std::size_t height,
                                                      const std::uint32_t radius) {
    validate_field_shape(guide, width, height);
    std::vector<float> squared(guide.size());
    parallel_for_scalar_rows(height,
                             [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
                                 for (std::size_t y = first_row; y < past_last_row; ++y) {
                                     const std::size_t row = y * width;
                                     for (std::size_t x = 0U; x < width; ++x) {
                                         const double value = guide[row + x];
                                         squared[row + x] = static_cast<float>(value * value);
                                     }
                                 }
                             });

    std::vector<float> mean = box_mean_scalar(guide, width, height, radius);
    std::vector<float> variance = box_mean_scalar(squared, width, height, radius);
    parallel_for_scalar_rows(
        height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = y * width;
                for (std::size_t x = 0U; x < width; ++x) {
                    const std::size_t pixel = row + x;
                    variance[pixel] = static_cast<float>(
                        std::max(0.0, static_cast<double>(variance[pixel]) -
                                          static_cast<double>(mean[pixel]) * mean[pixel]));
                }
            }
        });
    return PreparedGuidedFilter(std::move(mean), std::move(variance), width, height, radius);
}

std::vector<float> apply_guided_self_filter(const std::span<const float> guide,
                                            const PreparedGuidedFilter& prepared,
                                            const double epsilon) {
    validate_guided_filter_inputs(guide, prepared, epsilon);
    std::vector<float> a(guide.size());
    std::vector<float> b(guide.size());
    parallel_for_scalar_rows(
        prepared.height_, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = y * prepared.width_;
                for (std::size_t x = 0U; x < prepared.width_; ++x) {
                    const std::size_t pixel = row + x;
                    const double variance = prepared.variance_[pixel];
                    const double coefficient = variance / (variance + epsilon);
                    a[pixel] = static_cast<float>(coefficient);
                    b[pixel] = static_cast<float>(prepared.mean_[pixel] * (1.0 - coefficient));
                }
            }
        });
    const auto mean_a = box_mean_scalar(a, prepared.width_, prepared.height_, prepared.radius_);
    const auto mean_b = box_mean_scalar(b, prepared.width_, prepared.height_, prepared.radius_);
    std::vector<float> output(guide.size());
    parallel_for_scalar_rows(
        prepared.height_, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = y * prepared.width_;
                for (std::size_t x = 0U; x < prepared.width_; ++x) {
                    const std::size_t pixel = row + x;
                    output[pixel] = static_cast<float>(
                        static_cast<double>(mean_a[pixel]) * guide[pixel] + mean_b[pixel]);
                }
            }
        });
    return output;
}

std::vector<float> apply_guided_target_filter(const std::span<const float> guide,
                                              const PreparedGuidedFilter& prepared,
                                              const std::span<const float> target,
                                              const double epsilon) {
    validate_guided_filter_inputs(guide, prepared, epsilon);
    validate_field_shape(target, prepared.width_, prepared.height_);
    const auto mean_target =
        box_mean_scalar(target, prepared.width_, prepared.height_, prepared.radius_);
    std::vector<float> cross(guide.size());
    parallel_for_scalar_rows(
        prepared.height_, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = y * prepared.width_;
                for (std::size_t x = 0U; x < prepared.width_; ++x) {
                    const std::size_t pixel = row + x;
                    cross[pixel] =
                        static_cast<float>(static_cast<double>(guide[pixel]) * target[pixel]);
                }
            }
        });
    const auto mean_cross =
        box_mean_scalar(cross, prepared.width_, prepared.height_, prepared.radius_);
    std::vector<float> a(guide.size());
    std::vector<float> b(guide.size());
    parallel_for_scalar_rows(prepared.height_, [&](const std::uint32_t first_row,
                                                   const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = y * prepared.width_;
            for (std::size_t x = 0U; x < prepared.width_; ++x) {
                const std::size_t pixel = row + x;
                const double covariance =
                    static_cast<double>(mean_cross[pixel]) -
                    static_cast<double>(prepared.mean_[pixel]) * mean_target[pixel];
                const double coefficient =
                    covariance / (static_cast<double>(prepared.variance_[pixel]) + epsilon);
                a[pixel] = static_cast<float>(coefficient);
                b[pixel] =
                    static_cast<float>(mean_target[pixel] - coefficient * prepared.mean_[pixel]);
            }
        }
    });
    const auto mean_a = box_mean_scalar(a, prepared.width_, prepared.height_, prepared.radius_);
    const auto mean_b = box_mean_scalar(b, prepared.width_, prepared.height_, prepared.radius_);
    std::vector<float> output(target.size());
    parallel_for_scalar_rows(
        prepared.height_, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
            for (std::size_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = y * prepared.width_;
                for (std::size_t x = 0U; x < prepared.width_; ++x) {
                    const std::size_t pixel = row + x;
                    output[pixel] = static_cast<float>(
                        static_cast<double>(mean_a[pixel]) * guide[pixel] + mean_b[pixel]);
                }
            }
        });
    return output;
}

} // namespace shadow::image::detail
