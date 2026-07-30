#include "managed_raster_mask.hpp"

#include <shadow/image/edit_error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace shadow::image::detail {

namespace {

inline constexpr std::uint64_t maximum_managed_raster_mask_bytes = 64U * 1024U * 1024U;
inline constexpr std::uint64_t maximum_refinement_scratch_bytes = 64U * 1024U * 1024U;

[[noreturn]] void invalid_managed_raster(std::string message) {
    throw EditError(EditErrorCode::invalid_parameter, std::nullopt, std::move(message));
}

[[nodiscard]] std::uint16_t
little_endian_u16(const std::vector<std::uint8_t>& samples, const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(samples[offset])
           | static_cast<std::uint16_t>(static_cast<std::uint16_t>(samples[offset + 1U]) << 8U);
}

[[nodiscard]] double positive_binary16(const std::uint16_t bits) noexcept {
    const std::uint16_t exponent = static_cast<std::uint16_t>((bits >> 10U) & 0x1FU);
    const std::uint16_t fraction = static_cast<std::uint16_t>(bits & 0x03FFU);
    if (exponent == 0U) {
        return std::ldexp(static_cast<double>(fraction), -24);
    }
    return std::ldexp(
        static_cast<double>(1024U + static_cast<std::uint32_t>(fraction)),
        static_cast<int>(exponent) - 25
    );
}

[[nodiscard]] double
sample_at(const ManagedRasterMask& mask, const std::uint32_t x, const std::uint32_t y) noexcept {
    const std::size_t index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.raster_dimensions.width)
        + static_cast<std::size_t>(x);
    if (mask.encoding == ManagedRasterMaskEncoding::gray8) {
        return static_cast<double>(mask.samples[index]) / 255.0;
    }
    return positive_binary16(little_endian_u16(mask.samples, index * 2U));
}

[[nodiscard]] double sample_float_plane(
    const Dimensions dimensions,
    const std::vector<float>& samples,
    const double normalized_x,
    const double normalized_y
) noexcept {
    const double width = static_cast<double>(dimensions.width);
    const double height = static_cast<double>(dimensions.height);
    const double raster_x = std::clamp(normalized_x, 0.0, 1.0) * width - 0.5;
    const double raster_y = std::clamp(normalized_y, 0.0, 1.0) * height - 0.5;
    const double clamped_x = std::clamp(raster_x, 0.0, width - 1.0);
    const double clamped_y = std::clamp(raster_y, 0.0, height - 1.0);
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const std::uint32_t x1 = std::min(x0 + 1U, dimensions.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, dimensions.height - 1U);
    const double fraction_x = clamped_x - static_cast<double>(x0);
    const double fraction_y = clamped_y - static_cast<double>(y0);
    const auto at = [&](const std::uint32_t x, const std::uint32_t y) {
        return static_cast<double>(samples[static_cast<std::size_t>(y) * dimensions.width + x]);
    };
    const double top = std::lerp(at(x0, y0), at(x1, y0), fraction_x);
    const double bottom = std::lerp(at(x0, y1), at(x1, y1), fraction_x);
    return std::clamp(std::lerp(top, bottom, fraction_y), 0.0, 1.0);
}

[[nodiscard]] std::uint32_t refinement_radius(
    const Dimensions dimensions,
    const double amount,
    const std::uint32_t divisor
) noexcept {
    if (amount == 0.0) {
        return 0U;
    }
    const double shorter_edge = static_cast<double>(std::min(dimensions.width, dimensions.height));
    return std::max(
        1U,
        static_cast<std::uint32_t>(std::floor(std::abs(amount) * shorter_edge / divisor + 0.5))
    );
}

template <typename Value>
void horizontal_extreme(
    const std::vector<float>& input,
    std::vector<float>& output,
    const Dimensions dimensions,
    const std::uint32_t radius,
    Value value,
    const bool maximum
) {
    const auto width = static_cast<std::int64_t>(dimensions.width);
    const auto radius_signed = static_cast<std::int64_t>(radius);
    for (std::uint32_t row = 0U; row < dimensions.height; ++row) {
        std::deque<std::int64_t> window;
        const std::size_t row_offset = static_cast<std::size_t>(row) * dimensions.width;
        for (std::int64_t cursor = -radius_signed; cursor < width + radius_signed; ++cursor) {
            const auto source_x =
                static_cast<std::uint32_t>(std::clamp(cursor, std::int64_t{0}, width - 1));
            const float sample = value(input[row_offset + source_x]);
            while (!window.empty()) {
                const auto back_x = static_cast<std::uint32_t>(
                    std::clamp(window.back(), std::int64_t{0}, width - 1)
                );
                const float back = value(input[row_offset + back_x]);
                if (maximum ? back > sample : back < sample) {
                    break;
                }
                window.pop_back();
            }
            window.push_back(cursor);
            const std::int64_t minimum_cursor = cursor - 2 * radius_signed;
            while (!window.empty() && window.front() < minimum_cursor) {
                window.pop_front();
            }
            if (cursor >= radius_signed) {
                const auto output_x = static_cast<std::uint32_t>(cursor - radius_signed);
                const auto source = static_cast<std::uint32_t>(
                    std::clamp(window.front(), std::int64_t{0}, width - 1)
                );
                output[row_offset + output_x] = input[row_offset + source];
            }
        }
    }
}

void vertical_extreme(
    const std::vector<float>& input,
    std::vector<float>& output,
    const Dimensions dimensions,
    const std::uint32_t radius,
    const bool maximum
) {
    const auto height = static_cast<std::int64_t>(dimensions.height);
    const auto radius_signed = static_cast<std::int64_t>(radius);
    for (std::uint32_t column = 0U; column < dimensions.width; ++column) {
        std::deque<std::int64_t> window;
        for (std::int64_t cursor = -radius_signed; cursor < height + radius_signed; ++cursor) {
            const auto source_y =
                static_cast<std::uint32_t>(std::clamp(cursor, std::int64_t{0}, height - 1));
            const float sample =
                input[static_cast<std::size_t>(source_y) * dimensions.width + column];
            while (!window.empty()) {
                const auto back_y = static_cast<std::uint32_t>(
                    std::clamp(window.back(), std::int64_t{0}, height - 1)
                );
                const float back =
                    input[static_cast<std::size_t>(back_y) * dimensions.width + column];
                if (maximum ? back > sample : back < sample) {
                    break;
                }
                window.pop_back();
            }
            window.push_back(cursor);
            const std::int64_t minimum_cursor = cursor - 2 * radius_signed;
            while (!window.empty() && window.front() < minimum_cursor) {
                window.pop_front();
            }
            if (cursor >= radius_signed) {
                const auto output_y = static_cast<std::uint32_t>(cursor - radius_signed);
                const auto source = static_cast<std::uint32_t>(
                    std::clamp(window.front(), std::int64_t{0}, height - 1)
                );
                output[static_cast<std::size_t>(output_y) * dimensions.width + column] =
                    input[static_cast<std::size_t>(source) * dimensions.width + column];
            }
        }
    }
}

void apply_extreme_filter(
    std::vector<float>& samples,
    std::vector<float>& scratch,
    const Dimensions dimensions,
    const std::uint32_t radius,
    const bool maximum
) {
    horizontal_extreme(
        samples,
        scratch,
        dimensions,
        radius,
        [](const float value) { return value; },
        maximum
    );
    vertical_extreme(scratch, samples, dimensions, radius, maximum);
}

void horizontal_box_blur(
    const std::vector<float>& input,
    std::vector<float>& output,
    const Dimensions dimensions,
    const std::uint32_t radius
) {
    const auto radius_signed = static_cast<std::int64_t>(radius);
    const auto width = static_cast<std::int64_t>(dimensions.width);
    const double divisor = static_cast<double>(radius * 2U + 1U);
    for (std::uint32_t row = 0U; row < dimensions.height; ++row) {
        const std::size_t row_offset = static_cast<std::size_t>(row) * dimensions.width;
        double sum = 0.0;
        for (std::int64_t offset = -radius_signed; offset <= radius_signed; ++offset) {
            const auto x =
                static_cast<std::uint32_t>(std::clamp(offset, std::int64_t{0}, width - 1));
            sum += input[row_offset + x];
        }
        for (std::uint32_t column = 0U; column < dimensions.width; ++column) {
            output[row_offset + column] = static_cast<float>(sum / divisor);
            const auto remove_x = static_cast<std::uint32_t>(std::clamp(
                static_cast<std::int64_t>(column) - radius_signed,
                std::int64_t{0},
                width - 1
            ));
            const auto add_x = static_cast<std::uint32_t>(std::clamp(
                static_cast<std::int64_t>(column) + radius_signed + 1,
                std::int64_t{0},
                width - 1
            ));
            sum += input[row_offset + add_x] - input[row_offset + remove_x];
        }
    }
}

void vertical_box_blur(
    const std::vector<float>& input,
    std::vector<float>& output,
    const Dimensions dimensions,
    const std::uint32_t radius
) {
    const auto radius_signed = static_cast<std::int64_t>(radius);
    const auto height = static_cast<std::int64_t>(dimensions.height);
    const double divisor = static_cast<double>(radius * 2U + 1U);
    for (std::uint32_t column = 0U; column < dimensions.width; ++column) {
        double sum = 0.0;
        for (std::int64_t offset = -radius_signed; offset <= radius_signed; ++offset) {
            const auto y =
                static_cast<std::uint32_t>(std::clamp(offset, std::int64_t{0}, height - 1));
            sum += input[static_cast<std::size_t>(y) * dimensions.width + column];
        }
        for (std::uint32_t row = 0U; row < dimensions.height; ++row) {
            output[static_cast<std::size_t>(row) * dimensions.width + column] =
                static_cast<float>(sum / divisor);
            const auto remove_y = static_cast<std::uint32_t>(std::clamp(
                static_cast<std::int64_t>(row) - radius_signed,
                std::int64_t{0},
                height - 1
            ));
            const auto add_y = static_cast<std::uint32_t>(std::clamp(
                static_cast<std::int64_t>(row) + radius_signed + 1,
                std::int64_t{0},
                height - 1
            ));
            sum += input[static_cast<std::size_t>(add_y) * dimensions.width + column]
                   - input[static_cast<std::size_t>(remove_y) * dimensions.width + column];
        }
    }
}

void apply_box_blur(
    std::vector<float>& samples,
    std::vector<float>& scratch,
    const Dimensions dimensions,
    const std::uint32_t radius
) {
    horizontal_box_blur(samples, scratch, dimensions, radius);
    vertical_box_blur(scratch, samples, dimensions, radius);
}

} // namespace

void validate_managed_raster_mask(const ManagedRasterMask& mask) {
    if (mask.raster_dimensions.width == 0U || mask.raster_dimensions.height == 0U
        || mask.coordinate_dimensions.width == 0U || mask.coordinate_dimensions.height == 0U) {
        invalid_managed_raster(
            "managed raster mask dimensions and coordinate extent must be non-zero"
        );
    }
    const std::uint64_t sample_count = mask.raster_dimensions.pixel_count();
    std::uint64_t bytes_per_sample = 0U;
    switch (mask.encoding) {
    case ManagedRasterMaskEncoding::gray8:
        bytes_per_sample = 1U;
        break;
    case ManagedRasterMaskEncoding::gray16_float:
        bytes_per_sample = 2U;
        break;
    default:
        invalid_managed_raster("managed raster mask encoding is unsupported");
    }
    if (sample_count > maximum_managed_raster_mask_bytes / bytes_per_sample) {
        invalid_managed_raster("managed raster mask exceeds the 64 MiB execution budget");
    }
    const std::uint64_t expected_bytes = sample_count * bytes_per_sample;
    if (expected_bytes != static_cast<std::uint64_t>(mask.samples.size())) {
        invalid_managed_raster(
            "managed raster mask payload is not tightly packed for its dimensions and encoding"
        );
    }
    if (mask.encoding == ManagedRasterMaskEncoding::gray16_float) {
        for (std::size_t offset = 0U; offset < mask.samples.size(); offset += 2U) {
            // Positive binary16 bit patterns are monotonically ordered. 0x3c00
            // is exactly 1.0, so this one comparison rejects negative values,
            // NaN/Inf, and finite coverage greater than one.
            if (little_endian_u16(mask.samples, offset) > 0x3C00U) {
                invalid_managed_raster(
                    "managed raster mask binary16 samples must be finite and in [0, 1]"
                );
            }
        }
    }
}

double sample_managed_raster_mask(
    const ManagedRasterMask& mask,
    const double normalized_x,
    const double normalized_y
) noexcept {
    const double width = static_cast<double>(mask.raster_dimensions.width);
    const double height = static_cast<double>(mask.raster_dimensions.height);
    const double raster_x = std::clamp(normalized_x, 0.0, 1.0) * width - 0.5;
    const double raster_y = std::clamp(normalized_y, 0.0, 1.0) * height - 0.5;
    const double clamped_x = std::clamp(raster_x, 0.0, width - 1.0);
    const double clamped_y = std::clamp(raster_y, 0.0, height - 1.0);
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const std::uint32_t x1 = std::min(x0 + 1U, mask.raster_dimensions.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, mask.raster_dimensions.height - 1U);
    const double fraction_x = clamped_x - static_cast<double>(x0);
    const double fraction_y = clamped_y - static_cast<double>(y0);
    const double top = std::lerp(sample_at(mask, x0, y0), sample_at(mask, x1, y0), fraction_x);
    const double bottom = std::lerp(sample_at(mask, x0, y1), sample_at(mask, x1, y1), fraction_x);
    return std::clamp(std::lerp(top, bottom, fraction_y), 0.0, 1.0);
}

bool RefinedManagedRasterMask::valid() const noexcept {
    return dimensions.width > 0U && dimensions.height > 0U
           && dimensions.pixel_count() == static_cast<std::uint64_t>(samples.size());
}

RefinedManagedRasterMask refine_managed_raster_mask(
    const ManagedRasterMask& mask,
    const double expansion,
    const double feather
) {
    validate_managed_raster_mask(mask);
    if (!std::isfinite(expansion) || expansion < -1.0 || expansion > 1.0 || !std::isfinite(feather)
        || feather < 0.0 || feather > 1.0) {
        invalid_managed_raster("managed raster refinement controls must be finite and in range");
    }
    const std::uint64_t sample_count = mask.raster_dimensions.pixel_count();
    if (sample_count > maximum_refinement_scratch_bytes / (sizeof(float) * 2U)) {
        invalid_managed_raster("managed raster refinement exceeds the 64 MiB peak scratch budget");
    }
    RefinedManagedRasterMask refined{
        .dimensions = mask.raster_dimensions,
        .samples = std::vector<float>(static_cast<std::size_t>(sample_count)),
    };
    for (std::uint32_t row = 0U; row < mask.raster_dimensions.height; ++row) {
        for (std::uint32_t column = 0U; column < mask.raster_dimensions.width; ++column) {
            refined.samples[static_cast<std::size_t>(row) * mask.raster_dimensions.width + column] =
                static_cast<float>(sample_at(mask, column, row));
        }
    }
    std::vector<float> scratch(refined.samples.size());
    const std::uint32_t expansion_radius = refinement_radius(mask.raster_dimensions, expansion, 8U);
    if (expansion_radius > 0U) {
        apply_extreme_filter(
            refined.samples,
            scratch,
            refined.dimensions,
            expansion_radius,
            expansion > 0.0
        );
    }
    const std::uint32_t feather_radius = refinement_radius(mask.raster_dimensions, feather, 16U);
    if (feather_radius > 0U) {
        apply_box_blur(refined.samples, scratch, refined.dimensions, feather_radius);
    }
    return refined;
}

double sample_refined_managed_raster_mask(
    const RefinedManagedRasterMask& mask,
    const double normalized_x,
    const double normalized_y
) noexcept {
    if (!mask.valid()) {
        return 0.0;
    }
    return sample_float_plane(mask.dimensions, mask.samples, normalized_x, normalized_y);
}

} // namespace shadow::image::detail
