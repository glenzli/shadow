#include <shadow/image/edit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace shadow::image {
namespace {

[[noreturn]] void invalid_geometry(const std::string_view detail) {
    throw DecodeError(
        DecodeErrorCode::invalid_request,
        0,
        "photo geometry " + std::string(detail)
    );
}

[[nodiscard]] std::size_t rgb_sample_count(const Dimensions dimensions) {
    const std::uint64_t samples = static_cast<std::uint64_t>(dimensions.width)
        * static_cast<std::uint64_t>(dimensions.height) * 3U;
    if (samples == 0U || samples > std::numeric_limits<std::size_t>::max()) {
        invalid_geometry("dimensions overflow the RGB sample address space");
    }
    return static_cast<std::size_t>(samples);
}

void validate_image(const FloatRgbImage& image, const std::string_view role) {
    if (
        image.dimensions.width == 0U || image.dimensions.height == 0U
        || image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || image.transfer_function != TransferFunction::linear
        || image.row_stride_bytes
            != static_cast<std::size_t>(image.dimensions.width) * 3U * sizeof(float)
        || image.samples.size() != rgb_sample_count(image.dimensions)
    ) {
        invalid_geometry(role);
    }
}

[[nodiscard]] std::uint32_t crop_start(
    const std::uint32_t source_extent,
    const double normalized
) {
    const double extent = static_cast<double>(source_extent);
    return static_cast<std::uint32_t>(std::clamp(
        std::floor(normalized * extent),
        0.0,
        extent - 1.0
    ));
}

[[nodiscard]] std::uint32_t crop_end(
    const std::uint32_t source_extent,
    const double normalized
) {
    const double extent = static_cast<double>(source_extent);
    return static_cast<std::uint32_t>(std::clamp(
        std::ceil(normalized * extent),
        1.0,
        extent
    ));
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
        invalid_geometry("output tile lies outside the geometry canvas");
    }
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> source_coordinate_for_output(
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    const std::uint32_t output_x,
    const std::uint32_t output_y
) {
    const std::uint32_t crop_width = layout.source_crop.width;
    const std::uint32_t crop_height = layout.source_crop.height;
    std::uint32_t crop_x = 0U;
    std::uint32_t crop_y = 0U;
    switch (geometry.quarter_turn) {
    case PhotoQuarterTurn::zero:
        crop_x = output_x;
        crop_y = output_y;
        break;
    case PhotoQuarterTurn::clockwise_90:
        crop_x = output_y;
        crop_y = crop_height - 1U - output_x;
        break;
    case PhotoQuarterTurn::clockwise_180:
        crop_x = crop_width - 1U - output_x;
        crop_y = crop_height - 1U - output_y;
        break;
    case PhotoQuarterTurn::clockwise_270:
        crop_x = crop_width - 1U - output_y;
        crop_y = output_x;
        break;
    }
    if (geometry.flip_horizontal) {
        crop_x = crop_width - 1U - crop_x;
    }
    if (geometry.flip_vertical) {
        crop_y = crop_height - 1U - crop_y;
    }
    return {
        layout.source_crop.x + crop_x,
        layout.source_crop.y + crop_y,
    };
}

} // namespace

void validate_photo_geometry(const PhotoGeometry& geometry) {
    for (const double value : {
             geometry.crop_left,
             geometry.crop_top,
             geometry.crop_right,
             geometry.crop_bottom,
         }) {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            invalid_geometry("crop edges must be finite normalized values");
        }
    }
    if (geometry.crop_left >= geometry.crop_right || geometry.crop_top >= geometry.crop_bottom) {
        invalid_geometry("crop must retain non-zero width and height");
    }
    switch (geometry.quarter_turn) {
    case PhotoQuarterTurn::zero:
    case PhotoQuarterTurn::clockwise_90:
    case PhotoQuarterTurn::clockwise_180:
    case PhotoQuarterTurn::clockwise_270:
        return;
    }
    invalid_geometry("uses an unsupported quarter-turn value");
}

PhotoGeometryLayout photo_geometry_layout(
    const Dimensions source_dimensions,
    const PhotoGeometry& geometry
) {
    validate_photo_geometry(geometry);
    if (source_dimensions.width == 0U || source_dimensions.height == 0U) {
        invalid_geometry("requires non-zero source dimensions");
    }
    const std::uint32_t left = crop_start(source_dimensions.width, geometry.crop_left);
    const std::uint32_t top = crop_start(source_dimensions.height, geometry.crop_top);
    const std::uint32_t right = crop_end(source_dimensions.width, geometry.crop_right);
    const std::uint32_t bottom = crop_end(source_dimensions.height, geometry.crop_bottom);
    if (right <= left || bottom <= top) {
        invalid_geometry("crop has no addressable source pixels");
    }
    const GeometryPixelRect crop{
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };
    const bool transpose = geometry.quarter_turn == PhotoQuarterTurn::clockwise_90
        || geometry.quarter_turn == PhotoQuarterTurn::clockwise_270;
    return PhotoGeometryLayout{
        .source_crop = crop,
        .output_dimensions = Dimensions{
            .width = transpose ? crop.height : crop.width,
            .height = transpose ? crop.width : crop.height,
        },
    };
}

GeometryPixelRect photo_geometry_source_rect_for_output(
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    const GeometryPixelRect output_rect
) {
    validate_photo_geometry(geometry);
    if (layout.source_crop.width == 0U || layout.source_crop.height == 0U) {
        invalid_geometry("layout has an empty source crop");
    }
    validate_output_rect(output_rect, layout.output_dimensions);
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 4U> corners{
        source_coordinate_for_output(layout, geometry, output_rect.x, output_rect.y),
        source_coordinate_for_output(
            layout,
            geometry,
            output_rect.x + output_rect.width - 1U,
            output_rect.y
        ),
        source_coordinate_for_output(
            layout,
            geometry,
            output_rect.x,
            output_rect.y + output_rect.height - 1U
        ),
        source_coordinate_for_output(
            layout,
            geometry,
            output_rect.x + output_rect.width - 1U,
            output_rect.y + output_rect.height - 1U
        ),
    };
    std::uint32_t min_x = corners.front().first;
    std::uint32_t max_x = min_x;
    std::uint32_t min_y = corners.front().second;
    std::uint32_t max_y = min_y;
    for (const auto& [x, y] : corners) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    return GeometryPixelRect{
        .x = min_x,
        .y = min_y,
        .width = max_x - min_x + 1U,
        .height = max_y - min_y + 1U,
    };
}

FloatRgbImage apply_photo_geometry_tile(
    const FloatRgbImage& source_tile,
    const GeometryPixelRect source_tile_rect,
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    const GeometryPixelRect output_rect
) {
    validate_image(source_tile, "requires a contiguous linear RGB source tile");
    validate_output_rect(output_rect, layout.output_dimensions);
    if (
        source_tile_rect.width != source_tile.dimensions.width
        || source_tile_rect.height != source_tile.dimensions.height
    ) {
        invalid_geometry("source tile rectangle does not describe its RGB raster");
    }
    const GeometryPixelRect core =
        photo_geometry_source_rect_for_output(layout, geometry, output_rect);
    if (
        core.x < source_tile_rect.x || core.y < source_tile_rect.y
        || core.width > source_tile_rect.width - (core.x - source_tile_rect.x)
        || core.height > source_tile_rect.height - (core.y - source_tile_rect.y)
    ) {
        invalid_geometry("source tile does not contain the mapped output rectangle");
    }

    FloatRgbImage output;
    output.dimensions = Dimensions{output_rect.width, output_rect.height};
    output.row_stride_bytes = static_cast<std::size_t>(output_rect.width) * 3U * sizeof(float);
    output.pixel_format = source_tile.pixel_format;
    output.transfer_function = source_tile.transfer_function;
    output.reference = source_tile.reference;
    output.working_space = source_tile.working_space;
    const bool transpose = geometry.quarter_turn == PhotoQuarterTurn::clockwise_90
        || geometry.quarter_turn == PhotoQuarterTurn::clockwise_270;
    output.level_zero_to_raster_scale_x = transpose
        ? source_tile.level_zero_to_raster_scale_y
        : source_tile.level_zero_to_raster_scale_x;
    output.level_zero_to_raster_scale_y = transpose
        ? source_tile.level_zero_to_raster_scale_x
        : source_tile.level_zero_to_raster_scale_y;
    output.samples.resize(rgb_sample_count(output.dimensions));

    for (std::uint32_t y = 0U; y < output_rect.height; ++y) {
        for (std::uint32_t x = 0U; x < output_rect.width; ++x) {
            const auto [source_x, source_y] = source_coordinate_for_output(
                layout,
                geometry,
                output_rect.x + x,
                output_rect.y + y
            );
            const std::uint32_t local_x = source_x - source_tile_rect.x;
            const std::uint32_t local_y = source_y - source_tile_rect.y;
            const std::size_t source_index =
                (static_cast<std::size_t>(local_y) * source_tile.dimensions.width + local_x) * 3U;
            const std::size_t output_index =
                (static_cast<std::size_t>(y) * output.dimensions.width + x) * 3U;
            std::copy_n(
                source_tile.samples.begin() + static_cast<std::ptrdiff_t>(source_index),
                3U,
                output.samples.begin() + static_cast<std::ptrdiff_t>(output_index)
            );
        }
    }
    return output;
}

FloatRgbImage apply_photo_geometry(
    const FloatRgbImage& source,
    const PhotoGeometry& geometry
) {
    const PhotoGeometryLayout layout = photo_geometry_layout(source.dimensions, geometry);
    return apply_photo_geometry_tile(
        source,
        GeometryPixelRect{
            .x = 0U,
            .y = 0U,
            .width = source.dimensions.width,
            .height = source.dimensions.height,
        },
        layout,
        geometry,
        GeometryPixelRect{
            .x = 0U,
            .y = 0U,
            .width = layout.output_dimensions.width,
            .height = layout.output_dimensions.height,
        }
    );
}

} // namespace shadow::image
