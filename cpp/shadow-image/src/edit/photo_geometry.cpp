#include <shadow/image/decoder_error.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
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

[[nodiscard]] bool is_transposed(const PhotoQuarterTurn quarter_turn) noexcept {
    return quarter_turn == PhotoQuarterTurn::clockwise_90
        || quarter_turn == PhotoQuarterTurn::clockwise_270;
}

[[nodiscard]] Dimensions oriented_crop_dimensions(
    const GeometryPixelRect crop,
    const PhotoGeometry& geometry
) noexcept {
    return Dimensions{
        .width = is_transposed(geometry.quarter_turn) ? crop.height : crop.width,
        .height = is_transposed(geometry.quarter_turn) ? crop.width : crop.height,
    };
}

[[nodiscard]] Dimensions auto_crop_dimensions(
    const GeometryPixelRect crop,
    const PhotoGeometry& geometry
) {
    const Dimensions oriented = oriented_crop_dimensions(crop, geometry);
    if (geometry.straighten_degrees == 0.0) {
        return oriented;
    }

    // Fine rotation is centered on the original crop. Preserve that crop's
    // aspect ratio while shrinking the final canvas until each of its corners
    // inverse-maps into source pixels. This is the familiar crop-tool
    // auto-crop: it removes the empty triangles without creating another
    // persistent rectangle or changing the user's chosen aspect.
    const double width = static_cast<double>(oriented.width);
    const double height = static_cast<double>(oriented.height);
    const double angle = geometry.straighten_degrees
        * 3.141592653589793238462643383279502884 / 180.0;
    const double cosine = std::abs(std::cos(angle));
    const double sine = std::abs(std::sin(angle));
    const double scale = std::min(
        width / (cosine * width + sine * height),
        height / (sine * width + cosine * height)
    );
    const auto retained_extent = [](const double value) {
        return static_cast<std::uint32_t>(std::max(1.0, std::floor(value)));
    };
    return Dimensions{
        .width = retained_extent(width * scale),
        .height = retained_extent(height * scale),
    };
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

struct ContinuousCoordinate final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] ContinuousCoordinate source_coordinate_for_output(
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    const std::uint32_t output_x,
    const std::uint32_t output_y
) {
    const std::uint32_t crop_width = layout.source_crop.width;
    const std::uint32_t crop_height = layout.source_crop.height;
    const double output_width = static_cast<double>(layout.output_dimensions.width);
    const double output_height = static_cast<double>(layout.output_dimensions.height);
    const Dimensions full_oriented = oriented_crop_dimensions(layout.source_crop, geometry);
    const double full_oriented_width = static_cast<double>(full_oriented.width);
    const double full_oriented_height = static_cast<double>(full_oriented.height);
    const double angle = geometry.straighten_degrees
        * 3.141592653589793238462643383279502884 / 180.0;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double output_dx = static_cast<double>(output_x) + 0.5 - output_width * 0.5;
    const double output_dy = static_cast<double>(output_y) + 0.5 - output_height * 0.5;
    // Inverse-map the clockwise display rotation so every output pixel samples
    // the immutable source raster exactly once.
    const double oriented_x =
        std::fma(cosine, output_dx, sine * output_dy) + full_oriented_width * 0.5;
    const double oriented_y =
        std::fma(-sine, output_dx, cosine * output_dy) + full_oriented_height * 0.5;

    double crop_x = 0.0;
    double crop_y = 0.0;
    switch (geometry.quarter_turn) {
    case PhotoQuarterTurn::zero:
        crop_x = oriented_x;
        crop_y = oriented_y;
        break;
    case PhotoQuarterTurn::clockwise_90:
        crop_x = oriented_y;
        crop_y = static_cast<double>(crop_height) - oriented_x;
        break;
    case PhotoQuarterTurn::clockwise_180:
        crop_x = static_cast<double>(crop_width) - oriented_x;
        crop_y = static_cast<double>(crop_height) - oriented_y;
        break;
    case PhotoQuarterTurn::clockwise_270:
        crop_x = static_cast<double>(crop_width) - oriented_y;
        crop_y = oriented_x;
        break;
    }
    if (geometry.flip_horizontal) {
        crop_x = static_cast<double>(crop_width) - crop_x;
    }
    if (geometry.flip_vertical) {
        crop_y = static_cast<double>(crop_height) - crop_y;
    }
    return ContinuousCoordinate{
        .x = static_cast<double>(layout.source_crop.x) + crop_x - 0.5,
        .y = static_cast<double>(layout.source_crop.y) + crop_y - 0.5,
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
    if (!std::isfinite(geometry.straighten_degrees)
        || geometry.straighten_degrees < -45.0 || geometry.straighten_degrees > 45.0) {
        invalid_geometry("straighten angle must be finite and in [-45, 45] degrees");
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
    return PhotoGeometryLayout{
        .source_crop = crop,
        .output_dimensions = auto_crop_dimensions(crop, geometry),
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
    const std::array<ContinuousCoordinate, 4U> corners{
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
    double min_x = corners.front().x;
    double max_x = min_x;
    double min_y = corners.front().y;
    double max_y = min_y;
    for (const auto& corner : corners) {
        min_x = std::min(min_x, corner.x);
        max_x = std::max(max_x, corner.x);
        min_y = std::min(min_y, corner.y);
        max_y = std::max(max_y, corner.y);
    }
    const auto crop_right = layout.source_crop.x + layout.source_crop.width - 1U;
    const auto crop_bottom = layout.source_crop.y + layout.source_crop.height - 1U;
    const auto bounded_floor = [](const double value, const std::uint32_t lower,
                                  const std::uint32_t upper) {
        return static_cast<std::uint32_t>(std::clamp(
            std::floor(value),
            static_cast<double>(lower),
            static_cast<double>(upper)
        ));
    };
    const auto bounded_ceil = [](const double value, const std::uint32_t lower,
                                 const std::uint32_t upper) {
        return static_cast<std::uint32_t>(std::clamp(
            std::ceil(value),
            static_cast<double>(lower),
            static_cast<double>(upper)
        ));
    };
    const std::uint32_t source_left =
        bounded_floor(min_x, layout.source_crop.x, crop_right);
    const std::uint32_t source_right =
        bounded_ceil(max_x, layout.source_crop.x, crop_right);
    const std::uint32_t source_top =
        bounded_floor(min_y, layout.source_crop.y, crop_bottom);
    const std::uint32_t source_bottom =
        bounded_ceil(max_y, layout.source_crop.y, crop_bottom);
    return GeometryPixelRect{
        .x = source_left,
        .y = source_top,
        .width = source_right - source_left + 1U,
        .height = source_bottom - source_top + 1U,
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
    const bool transpose = is_transposed(geometry.quarter_turn);
    output.level_zero_to_raster_scale_x = transpose
        ? source_tile.level_zero_to_raster_scale_y
        : source_tile.level_zero_to_raster_scale_x;
    output.level_zero_to_raster_scale_y = transpose
        ? source_tile.level_zero_to_raster_scale_x
        : source_tile.level_zero_to_raster_scale_y;
    output.samples.resize(rgb_sample_count(output.dimensions));

    for (std::uint32_t y = 0U; y < output_rect.height; ++y) {
        for (std::uint32_t x = 0U; x < output_rect.width; ++x) {
            const auto source = source_coordinate_for_output(
                layout,
                geometry,
                output_rect.x + x,
                output_rect.y + y
            );
            const std::size_t output_index =
                (static_cast<std::size_t>(y) * output.dimensions.width + x) * 3U;
            const double crop_left = static_cast<double>(layout.source_crop.x);
            const double crop_top = static_cast<double>(layout.source_crop.y);
            const double crop_right = static_cast<double>(
                layout.source_crop.x + layout.source_crop.width - 1U
            );
            const double crop_bottom = static_cast<double>(
                layout.source_crop.y + layout.source_crop.height - 1U
            );
            if (source.x < crop_left || source.x > crop_right
                || source.y < crop_top || source.y > crop_bottom) {
                std::fill_n(
                    output.samples.begin() + static_cast<std::ptrdiff_t>(output_index),
                    3U,
                    0.0F
                );
                continue;
            }
            const std::uint32_t source_x0 =
                static_cast<std::uint32_t>(std::floor(source.x));
            const std::uint32_t source_y0 =
                static_cast<std::uint32_t>(std::floor(source.y));
            // A tile boundary can land exactly on a source-pixel centre. In
            // that case bilinear interpolation has no right/bottom weight,
            // and requesting either neighbour would both be unnecessary and
            // fall outside a minimally sized detail tile. Keep the sampling
            // footprint aligned with photo_geometry_source_rect_for_output:
            // it includes a second pixel only when a fractional coordinate
            // actually needs it.
            const double fraction_x = source.x - static_cast<double>(source_x0);
            const double fraction_y = source.y - static_cast<double>(source_y0);
            const std::uint32_t source_x1 = std::min(
                fraction_x == 0.0 ? source_x0 : source_x0 + 1U,
                layout.source_crop.x + layout.source_crop.width - 1U
            );
            const std::uint32_t source_y1 = std::min(
                fraction_y == 0.0 ? source_y0 : source_y0 + 1U,
                layout.source_crop.y + layout.source_crop.height - 1U
            );
            const auto source_index = [&](const std::uint32_t source_x,
                                          const std::uint32_t source_y) {
                const std::uint32_t local_x = source_x - source_tile_rect.x;
                const std::uint32_t local_y = source_y - source_tile_rect.y;
                return (static_cast<std::size_t>(local_y)
                            * source_tile.dimensions.width + local_x) * 3U;
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
                output.samples[output_index + channel] = static_cast<float>(
                    std::lerp(top, bottom, fraction_y)
                );
            }
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
