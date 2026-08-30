#include "image_completion.hpp"

#include <shadow/image/edit_error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace shadow::image::detail {

namespace {

constexpr std::size_t maximum_completion_patches = 32U;
constexpr std::uint32_t maximum_completion_edge = 2'048U;
constexpr std::size_t maximum_completion_bytes = 16U * 1'024U * 1'024U;

[[nodiscard]] double decode_srgb(const std::uint8_t value) noexcept {
    const double encoded = static_cast<double>(value) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] bool normalized(const double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

} // namespace

void validate_image_completion(const ImageCompletionAdjustment& adjustment) {
    if (adjustment.patches.empty() || adjustment.patches.size() > maximum_completion_patches) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "AI completion must contain 1 through 32 accepted patches"
        );
    }
    for (const auto& patch : adjustment.patches) {
        const std::size_t width = patch.raster_width;
        const std::size_t height = patch.raster_height;
        const bool byte_extent_valid =
            width > 0U && height > 0U && width <= maximum_completion_edge
            && height <= maximum_completion_edge
            && width <= std::numeric_limits<std::size_t>::max() / height
            && width * height <= std::numeric_limits<std::size_t>::max() / 4U;
        const std::size_t expected = byte_extent_valid ? width * height * 4U : 0U;
        if (!byte_extent_valid || expected > maximum_completion_bytes
            || patch.rgba8.size() != expected || patch.coordinate_width == 0U
            || patch.coordinate_height == 0U || !normalized(patch.bounds_left)
            || !normalized(patch.bounds_top) || !normalized(patch.bounds_right)
            || !normalized(patch.bounds_bottom) || patch.bounds_left >= patch.bounds_right
            || patch.bounds_top >= patch.bounds_bottom || !normalized(patch.strength)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "AI completion patch is malformed or outside its bounded RGBA8 contract"
            );
        }
    }
}

void apply_image_completion(
    FloatRgbImage& image,
    const ImageCompletionAdjustment& adjustment,
    const AdjustmentExecutionContext& context
) {
    validate_image_completion(adjustment);
    const Dimensions full =
        context.full_dimensions.width == 0U || context.full_dimensions.height == 0U
            ? image.dimensions
            : context.full_dimensions;
    const std::size_t row_floats = image.row_stride_bytes / sizeof(float);
    for (const auto& patch : adjustment.patches) {
        if (patch.strength == 0.0) {
            continue;
        }
        const auto patch_left = static_cast<std::uint32_t>(
            std::floor(patch.bounds_left * static_cast<double>(full.width))
        );
        const auto patch_top = static_cast<std::uint32_t>(
            std::floor(patch.bounds_top * static_cast<double>(full.height))
        );
        const auto patch_right = std::min(
            full.width,
            static_cast<std::uint32_t>(
                std::ceil(patch.bounds_right * static_cast<double>(full.width))
            )
        );
        const auto patch_bottom = std::min(
            full.height,
            static_cast<std::uint32_t>(
                std::ceil(patch.bounds_bottom * static_cast<double>(full.height))
            )
        );
        const std::uint64_t tile_left = context.origin_x;
        const std::uint64_t tile_top = context.origin_y;
        const std::uint64_t tile_right = tile_left + image.dimensions.width;
        const std::uint64_t tile_bottom = tile_top + image.dimensions.height;
        const std::uint64_t intersection_left = std::max<std::uint64_t>(patch_left, tile_left);
        const std::uint64_t intersection_top = std::max<std::uint64_t>(patch_top, tile_top);
        const std::uint64_t intersection_right = std::min<std::uint64_t>(patch_right, tile_right);
        const std::uint64_t intersection_bottom =
            std::min<std::uint64_t>(patch_bottom, tile_bottom);
        if (intersection_left >= intersection_right || intersection_top >= intersection_bottom) {
            continue;
        }
        const auto local_left = static_cast<std::uint32_t>(intersection_left - tile_left);
        const auto local_top = static_cast<std::uint32_t>(intersection_top - tile_top);
        const auto local_right = static_cast<std::uint32_t>(intersection_right - tile_left);
        const auto local_bottom = static_cast<std::uint32_t>(intersection_bottom - tile_top);
        for (std::uint32_t local_y = local_top; local_y < local_bottom; ++local_y) {
            const std::uint32_t global_y = context.origin_y + local_y;
            const double normalized_y =
                (static_cast<double>(global_y) + 0.5) / static_cast<double>(full.height);
            if (normalized_y < patch.bounds_top || normalized_y >= patch.bounds_bottom) {
                continue;
            }
            const double patch_y =
                (normalized_y - patch.bounds_top) / (patch.bounds_bottom - patch.bounds_top);
            const std::uint32_t sample_y = std::min(
                patch.raster_height - 1U,
                static_cast<std::uint32_t>(patch_y * static_cast<double>(patch.raster_height))
            );
            for (std::uint32_t local_x = local_left; local_x < local_right; ++local_x) {
                const std::uint32_t global_x = context.origin_x + local_x;
                const double normalized_x =
                    (static_cast<double>(global_x) + 0.5) / static_cast<double>(full.width);
                if (normalized_x < patch.bounds_left || normalized_x >= patch.bounds_right) {
                    continue;
                }
                const double patch_x =
                    (normalized_x - patch.bounds_left) / (patch.bounds_right - patch.bounds_left);
                const std::uint32_t sample_x = std::min(
                    patch.raster_width - 1U,
                    static_cast<std::uint32_t>(patch_x * static_cast<double>(patch.raster_width))
                );
                const std::size_t patch_offset =
                    (static_cast<std::size_t>(sample_y) * patch.raster_width + sample_x) * 4U;
                const double alpha =
                    static_cast<double>(patch.rgba8[patch_offset + 3U]) / 255.0 * patch.strength;
                if (alpha <= 0.0) {
                    continue;
                }
                const std::size_t image_offset = static_cast<std::size_t>(local_y) * row_floats
                                                 + static_cast<std::size_t>(local_x) * 3U;
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    const double source = image.samples[image_offset + channel];
                    const double replacement = decode_srgb(patch.rgba8[patch_offset + channel]);
                    image.samples[image_offset + channel] =
                        static_cast<float>(source + (replacement - source) * alpha);
                }
            }
        }
    }
}

} // namespace shadow::image::detail
