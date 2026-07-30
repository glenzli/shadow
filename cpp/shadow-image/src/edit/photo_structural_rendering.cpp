#include <shadow/image/decoder_error.hpp>
#include <shadow/image/photo_structural_rendering.hpp>
#include <shadow/image/working_rgb.hpp>

#include "photo_geometry_sampling.hpp"
#include "photo_liquify_sampling.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace shadow::image {
namespace {

[[noreturn]] void invalid_structural_rendering(const std::string_view detail) {
    throw DecodeError(
        DecodeErrorCode::invalid_request,
        0,
        "photo structural rendering " + std::string(detail)
    );
}

[[nodiscard]] std::size_t rgb_sample_count(const Dimensions dimensions) {
    const std::uint64_t samples = static_cast<std::uint64_t>(dimensions.width)
        * static_cast<std::uint64_t>(dimensions.height) * 3U;
    if (samples == 0U || samples > std::numeric_limits<std::size_t>::max()) {
        invalid_structural_rendering("dimensions overflow the RGB sample address space");
    }
    return static_cast<std::size_t>(samples);
}

void validate_image(const FloatRgbImage& image) {
    if (
        image.dimensions.width == 0U || image.dimensions.height == 0U
        || image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || image.transfer_function != TransferFunction::linear
        || image.row_stride_bytes
            != static_cast<std::size_t>(image.dimensions.width) * 3U * sizeof(float)
        || image.samples.size() != rgb_sample_count(image.dimensions)
    ) {
        invalid_structural_rendering("requires a contiguous linear RGB source tile");
    }
}

void validate_output_rect(
    const GeometryPixelRect rect,
    const Dimensions dimensions
) {
    if (
        rect.width == 0U || rect.height == 0U || rect.x >= dimensions.width
        || rect.y >= dimensions.height || rect.width > dimensions.width - rect.x
        || rect.height > dimensions.height - rect.y
    ) {
        invalid_structural_rendering("output tile lies outside the Canvas");
    }
}

[[nodiscard]] bool contains(
    const GeometryPixelRect outer,
    const GeometryPixelRect inner
) noexcept {
    const std::uint64_t outer_right =
        static_cast<std::uint64_t>(outer.x) + outer.width;
    const std::uint64_t outer_bottom =
        static_cast<std::uint64_t>(outer.y) + outer.height;
    const std::uint64_t inner_right =
        static_cast<std::uint64_t>(inner.x) + inner.width;
    const std::uint64_t inner_bottom =
        static_cast<std::uint64_t>(inner.y) + inner.height;
    return inner.x >= outer.x && inner.y >= outer.y
        && inner_right <= outer_right && inner_bottom <= outer_bottom;
}

[[nodiscard]] std::uint32_t displacement_bound(
    const PreparedPhotoStructuralRendering& structural
) {
    if (!structural.liquify.has_value()) {
        return 0U;
    }
    const double value = std::ceil(structural.liquify->maximum_displacement_pixels);
    if (!std::isfinite(value)
        || value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        invalid_structural_rendering("Liquify displacement bound exceeds the addressable raster");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] GeometryPixelRect expand_rect(
    const GeometryPixelRect rect,
    const Dimensions dimensions,
    const std::uint32_t bound
) {
    const std::uint32_t left = std::min(rect.x, bound);
    const std::uint32_t top = std::min(rect.y, bound);
    const std::uint32_t right = std::min(
        dimensions.width - (rect.x + rect.width),
        bound
    );
    const std::uint32_t bottom = std::min(
        dimensions.height - (rect.y + rect.height),
        bound
    );
    return GeometryPixelRect{
        .x = rect.x - left,
        .y = rect.y - top,
        .width = rect.width + left + right,
        .height = rect.height + top + bottom,
    };
}

} // namespace

PreparedPhotoStructuralRendering prepare_photo_structural_rendering(
    const Dimensions source_dimensions,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) {
    PreparedPhotoStructuralRendering result{
        .source_dimensions = source_dimensions,
        .geometry = geometry,
        .geometry_layout = photo_geometry_layout(source_dimensions, geometry),
    };
    if (liquify != nullptr) {
        result.liquify = prepare_photo_liquify(source_dimensions, *liquify);
    }
    return result;
}

GeometryPixelRect photo_structural_source_rect_for_output(
    const PreparedPhotoStructuralRendering& structural,
    const GeometryPixelRect output_rect
) {
    const GeometryPixelRect geometry_core = photo_geometry_source_rect_for_output(
        structural.geometry_layout,
        structural.geometry,
        output_rect
    );
    return expand_rect(
        geometry_core,
        structural.source_dimensions,
        displacement_bound(structural)
    );
}

FloatRgbImage apply_photo_structural_rendering_tile(
    const FloatRgbImage& source_tile,
    const GeometryPixelRect source_tile_rect,
    const PreparedPhotoStructuralRendering& structural,
    const GeometryPixelRect output_rect
) {
    validate_image(source_tile);
    validate_output_rect(output_rect, structural.geometry_layout.output_dimensions);
    if (
        source_tile_rect.width != source_tile.dimensions.width
        || source_tile_rect.height != source_tile.dimensions.height
        || source_tile_rect.x >= structural.source_dimensions.width
        || source_tile_rect.y >= structural.source_dimensions.height
        || source_tile_rect.width
            > structural.source_dimensions.width - source_tile_rect.x
        || source_tile_rect.height
            > structural.source_dimensions.height - source_tile_rect.y
    ) {
        invalid_structural_rendering("source tile rectangle does not describe its RGB raster");
    }
    const GeometryPixelRect required =
        photo_structural_source_rect_for_output(structural, output_rect);
    if (!contains(source_tile_rect, required)) {
        invalid_structural_rendering("source tile does not contain the structural preimage");
    }

    FloatRgbImage output;
    output.dimensions = Dimensions{output_rect.width, output_rect.height};
    output.row_stride_bytes =
        static_cast<std::size_t>(output_rect.width) * 3U * sizeof(float);
    output.pixel_format = source_tile.pixel_format;
    output.transfer_function = source_tile.transfer_function;
    output.reference = source_tile.reference;
    output.working_space = source_tile.working_space;
    const bool transpose =
        detail::photo_geometry_is_transposed(structural.geometry.quarter_turn);
    output.level_zero_to_raster_scale_x = transpose
        ? source_tile.level_zero_to_raster_scale_y
        : source_tile.level_zero_to_raster_scale_x;
    output.level_zero_to_raster_scale_y = transpose
        ? source_tile.level_zero_to_raster_scale_x
        : source_tile.level_zero_to_raster_scale_y;
    output.samples.resize(rgb_sample_count(output.dimensions));

    const double crop_left = static_cast<double>(structural.geometry_layout.source_crop.x);
    const double crop_top = static_cast<double>(structural.geometry_layout.source_crop.y);
    const double crop_right = static_cast<double>(
        structural.geometry_layout.source_crop.x
        + structural.geometry_layout.source_crop.width - 1U
    );
    const double crop_bottom = static_cast<double>(
        structural.geometry_layout.source_crop.y
        + structural.geometry_layout.source_crop.height - 1U
    );
    const double maximum_x = static_cast<double>(structural.source_dimensions.width - 1U);
    const double maximum_y = static_cast<double>(structural.source_dimensions.height - 1U);
    for (std::uint32_t y = 0U; y < output_rect.height; ++y) {
        for (std::uint32_t x = 0U; x < output_rect.width; ++x) {
            const std::size_t output_index =
                (static_cast<std::size_t>(y) * output.dimensions.width + x) * 3U;
            const auto canvas_source = detail::photo_geometry_source_coordinate_for_output(
                structural.geometry_layout,
                structural.geometry,
                output_rect.x + x,
                output_rect.y + y
            );
            if (
                canvas_source.x < crop_left || canvas_source.x > crop_right
                || canvas_source.y < crop_top || canvas_source.y > crop_bottom
            ) {
                std::fill_n(
                    output.samples.begin() + static_cast<std::ptrdiff_t>(output_index),
                    3U,
                    0.0F
                );
                continue;
            }
            auto source_x = canvas_source.x;
            auto source_y = canvas_source.y;
            if (structural.liquify.has_value()) {
                const auto liquify_source = detail::inverse_photo_liquify_coordinate(
                    *structural.liquify,
                    source_x,
                    source_y
                );
                source_x = liquify_source.x;
                source_y = liquify_source.y;
            }
            source_x = std::clamp(source_x, 0.0, maximum_x);
            source_y = std::clamp(source_y, 0.0, maximum_y);
            const auto source_x0 = static_cast<std::uint32_t>(std::floor(source_x));
            const auto source_y0 = static_cast<std::uint32_t>(std::floor(source_y));
            const double fraction_x = source_x - static_cast<double>(source_x0);
            const double fraction_y = source_y - static_cast<double>(source_y0);
            const std::uint32_t source_x1 = std::min(
                fraction_x == 0.0 ? source_x0 : source_x0 + 1U,
                structural.source_dimensions.width - 1U
            );
            const std::uint32_t source_y1 = std::min(
                fraction_y == 0.0 ? source_y0 : source_y0 + 1U,
                structural.source_dimensions.height - 1U
            );
            const auto source_index = [&](const std::uint32_t sample_x,
                                          const std::uint32_t sample_y) {
                const std::uint32_t local_x = sample_x - source_tile_rect.x;
                const std::uint32_t local_y = sample_y - source_tile_rect.y;
                return (
                    static_cast<std::size_t>(local_y) * source_tile.dimensions.width + local_x
                ) * 3U;
            };
            const std::size_t index_00 = source_index(source_x0, source_y0);
            const std::size_t index_10 = source_index(source_x1, source_y0);
            const std::size_t index_01 = source_index(source_x0, source_y1);
            const std::size_t index_11 = source_index(source_x1, source_y1);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const double top = std::lerp(
                    static_cast<double>(source_tile.samples[index_00 + channel]),
                    static_cast<double>(source_tile.samples[index_10 + channel]),
                    fraction_x
                );
                const double bottom = std::lerp(
                    static_cast<double>(source_tile.samples[index_01 + channel]),
                    static_cast<double>(source_tile.samples[index_11 + channel]),
                    fraction_x
                );
                output.samples[output_index + channel] =
                    static_cast<float>(std::lerp(top, bottom, fraction_y));
            }
        }
    }
    return output;
}

FloatRgbImage apply_photo_structural_rendering(
    const FloatRgbImage& source,
    const PreparedPhotoStructuralRendering& structural
) {
    if (source.dimensions != structural.source_dimensions) {
        invalid_structural_rendering("prepared plan does not match the source raster");
    }
    return apply_photo_structural_rendering_tile(
        source,
        GeometryPixelRect{
            .x = 0U,
            .y = 0U,
            .width = source.dimensions.width,
            .height = source.dimensions.height,
        },
        structural,
        GeometryPixelRect{
            .x = 0U,
            .y = 0U,
            .width = structural.geometry_layout.output_dimensions.width,
            .height = structural.geometry_layout.output_dimensions.height,
        }
    );
}

} // namespace shadow::image
