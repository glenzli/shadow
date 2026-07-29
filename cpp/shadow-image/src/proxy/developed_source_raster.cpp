#include "developed_source_raster.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

namespace shadow::image::proxy_detail {

namespace {

[[nodiscard]] std::size_t validated_source_row_stride(const PixelBuffer& source) {
    if (source.bits_per_channel != 16U || (source.channels != 1U && source.channels != 3U)
        || source.dimensions.width == 0U || source.dimensions.height == 0U
        || source.primaries != RgbPrimaries::srgb_rec709_d65
        || source.transfer_function != RgbTransferFunction::linear
        || (source.reference != RgbBufferReference::processed_raw
            && source.reference != RgbBufferReference::decoded_raster)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "proxy renderer requires non-empty 16-bit standardized linear RGB with "
            "sRGB/Rec.709-D65 primaries"
        );
    }
    if (static_cast<std::uint64_t>(source.dimensions.width) * source.channels
        > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy source stride overflows");
    }
    const std::size_t minimum_stride =
        static_cast<std::size_t>(source.dimensions.width) * source.channels;
    if (source.row_stride_bytes % sizeof(std::uint16_t) != 0U
        || source.row_stride_bytes / sizeof(std::uint16_t) < minimum_stride) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "proxy source row stride is invalid");
    }
    const std::size_t row_stride = source.row_stride_bytes / sizeof(std::uint16_t);
    if (static_cast<std::uint64_t>(row_stride) * source.dimensions.height
        > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy source buffer overflows");
    }
    const std::size_t required_samples =
        row_stride * static_cast<std::size_t>(source.dimensions.height);
    if (source.samples.size() < required_samples) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "proxy source buffer is truncated");
    }
    return row_stride;
}

[[nodiscard]] std::size_t validated_scene_linear_row_stride(const SceneLinearRgbFrame& source) {
    if (!source.valid() || source.row_stride_bytes % sizeof(float) != 0U) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "proxy renderer received an invalid fp32 scene-linear RAW source"
        );
    }
    return source.row_stride_bytes / sizeof(float);
}

[[nodiscard]] std::uint16_t source_sample(
    const PixelBuffer& source,
    const std::size_t row_stride,
    const std::size_t x,
    const std::size_t y,
    const std::size_t channel
) {
    const std::size_t source_channel = source.channels == 1U ? 0U : channel;
    return source.samples[(y * row_stride) + (x * source.channels) + source_channel];
}

[[nodiscard]] WorkingRgbSpace linear_srgb_working_space() {
    return WorkingRgbSpace{
        .id = "srgb-d65-linear",
        .primaries =
            {
                Chromaticity{0.6400, 0.3300},
                Chromaticity{0.3000, 0.6000},
                Chromaticity{0.1500, 0.0600},
            },
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

void validate_resize_target(const Dimensions target) {
    if (target.width == 0U || target.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "developed source resize target must be non-empty"
        );
    }
}

void validate_crop_rect(const GeometryPixelRect rect, const Dimensions source_dimensions) {
    if (rect.width == 0U || rect.height == 0U || rect.x >= source_dimensions.width
        || rect.y >= source_dimensions.height || rect.width > source_dimensions.width - rect.x
        || rect.height > source_dimensions.height - rect.y) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "developed source crop must be non-empty and fully inside the source"
        );
    }
}

[[nodiscard]] FloatRgbImage
resize_processed_linear_to_working(const PixelBuffer& source, const Dimensions target) {
    const std::size_t source_stride = validated_source_row_stride(source);
    validate_resize_target(target);
    const std::size_t sample_count = checked_interleaved_rgb_sample_count(target);
    const std::uint64_t row_samples = static_cast<std::uint64_t>(target.width) * 3U;
    if (row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "scene-linear proxy row stride overflows the address space"
        );
    }
    if (sample_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "scene-linear proxy buffer exceeds the address space"
        );
    }
    FloatRgbImage output;
    output.dimensions = target;
    output.row_stride_bytes = static_cast<std::size_t>(row_samples) * sizeof(float);
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    // A processed RAW reference remains scene-referred after normalization. A decoded JPEG/SDR
    // HEIF reference has been transfer-decoded into linear working RGB, but retains the source's
    // display-referred appearance. Both share the editable linear graph; only their output
    // boundary differs.
    output.reference = source.reference == RgbBufferReference::processed_raw
                           ? ImageReference::scene_referred
                           : ImageReference::display_referred;
    output.working_space = linear_srgb_working_space();
    output.level_zero_to_raster_scale_x =
        static_cast<double>(target.width) / static_cast<double>(source.dimensions.width);
    output.level_zero_to_raster_scale_y =
        static_cast<double>(target.height) / static_cast<double>(source.dimensions.height);
    output.samples.resize(sample_count);

    const double scale_x =
        static_cast<double>(source.dimensions.width) / static_cast<double>(target.width);
    const double scale_y =
        static_cast<double>(source.dimensions.height) / static_cast<double>(target.height);

    for (std::uint32_t output_y = 0; output_y < target.height; ++output_y) {
        const double source_y =
            std::max(0.0, (static_cast<double>(output_y) + 0.5) * scale_y - 0.5);
        const auto y0 = static_cast<std::size_t>(source_y);
        const auto y1 = std::min(y0 + 1U, static_cast<std::size_t>(source.dimensions.height - 1U));
        const double fraction_y = source_y - static_cast<double>(y0);

        for (std::uint32_t output_x = 0; output_x < target.width; ++output_x) {
            const double source_x =
                std::max(0.0, (static_cast<double>(output_x) + 0.5) * scale_x - 0.5);
            const auto x0 = static_cast<std::size_t>(source_x);
            const auto x1 =
                std::min(x0 + 1U, static_cast<std::size_t>(source.dimensions.width - 1U));
            const double fraction_x = source_x - static_cast<double>(x0);
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * target.width + output_x) * 3U;

            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const auto linear_sample = [&, channel](const std::size_t x, const std::size_t y) {
                    return static_cast<double>(source_sample(source, source_stride, x, y, channel))
                           / 65'535.0;
                };
                const double top =
                    linear_sample(x0, y0) * (1.0 - fraction_x) + linear_sample(x1, y0) * fraction_x;
                const double bottom =
                    linear_sample(x0, y1) * (1.0 - fraction_x) + linear_sample(x1, y1) * fraction_x;
                output.samples[output_index + channel] =
                    static_cast<float>(top * (1.0 - fraction_y) + bottom * fraction_y);
            }
        }
    }
    return output;
}

[[nodiscard]] FloatRgbImage
resize_processed_linear_to_working(const SceneLinearRgbFrame& source, const Dimensions target) {
    const std::size_t source_stride = validated_scene_linear_row_stride(source);
    validate_resize_target(target);
    const std::size_t sample_count = checked_interleaved_rgb_sample_count(target);
    FloatRgbImage output;
    output.dimensions = target;
    output.row_stride_bytes = static_cast<std::size_t>(target.width) * 3U * sizeof(float);
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
    output.level_zero_to_raster_scale_x =
        static_cast<double>(target.width) / static_cast<double>(source.dimensions.width);
    output.level_zero_to_raster_scale_y =
        static_cast<double>(target.height) / static_cast<double>(source.dimensions.height);
    output.samples.resize(sample_count);
    const double scale_x =
        static_cast<double>(source.dimensions.width) / static_cast<double>(target.width);
    const double scale_y =
        static_cast<double>(source.dimensions.height) / static_cast<double>(target.height);
    for (std::uint32_t output_y = 0U; output_y < target.height; ++output_y) {
        const double source_y =
            std::max(0.0, (static_cast<double>(output_y) + 0.5) * scale_y - 0.5);
        const auto y0 = static_cast<std::size_t>(source_y);
        const auto y1 = std::min(y0 + 1U, static_cast<std::size_t>(source.dimensions.height - 1U));
        const double fraction_y = source_y - static_cast<double>(y0);
        for (std::uint32_t output_x = 0U; output_x < target.width; ++output_x) {
            const double source_x =
                std::max(0.0, (static_cast<double>(output_x) + 0.5) * scale_x - 0.5);
            const auto x0 = static_cast<std::size_t>(source_x);
            const auto x1 =
                std::min(x0 + 1U, static_cast<std::size_t>(source.dimensions.width - 1U));
            const double fraction_x = source_x - static_cast<double>(x0);
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * target.width + output_x) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const auto sample = [&, channel](const std::size_t x, const std::size_t y) {
                    return static_cast<double>(
                        source.samples[y * source_stride + x * 3U + channel]
                    );
                };
                const double top =
                    sample(x0, y0) * (1.0 - fraction_x) + sample(x1, y0) * fraction_x;
                const double bottom =
                    sample(x0, y1) * (1.0 - fraction_x) + sample(x1, y1) * fraction_x;
                output.samples[output_index + channel] =
                    static_cast<float>(top * (1.0 - fraction_y) + bottom * fraction_y);
            }
        }
    }
    return output;
}

[[nodiscard]] FloatRgbImage
crop_processed_linear_to_working(const PixelBuffer& source, const GeometryPixelRect rect) {
    const std::size_t source_stride = validated_source_row_stride(source);
    validate_crop_rect(rect, source.dimensions);
    const Dimensions tile_dimensions{rect.width, rect.height};
    const std::size_t sample_count = checked_interleaved_rgb_sample_count(tile_dimensions);
    const std::uint64_t row_samples = static_cast<std::uint64_t>(rect.width) * 3U;
    if (row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "detail tile row stride overflows the address space"
        );
    }
    if (sample_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "detail tile float buffer exceeds the address space"
        );
    }

    FloatRgbImage output;
    output.dimensions = tile_dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(row_samples) * sizeof(float);
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    output.reference = source.reference == RgbBufferReference::processed_raw
                           ? ImageReference::scene_referred
                           : ImageReference::display_referred;
    output.working_space = linear_srgb_working_space();
    output.level_zero_to_raster_scale_x = 1.0;
    output.level_zero_to_raster_scale_y = 1.0;
    output.samples.resize(sample_count);

    for (std::uint32_t output_y = 0; output_y < rect.height; ++output_y) {
        const std::size_t source_y = static_cast<std::size_t>(rect.y) + output_y;
        for (std::uint32_t output_x = 0; output_x < rect.width; ++output_x) {
            const std::size_t source_x = static_cast<std::size_t>(rect.x) + output_x;
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * rect.width + output_x) * 3U;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const double linear =
                    static_cast<double>(
                        source_sample(source, source_stride, source_x, source_y, channel)
                    )
                    / 65'535.0;
                output.samples[output_index + channel] = static_cast<float>(linear);
            }
        }
    }
    return output;
}

[[nodiscard]] FloatRgbImage
crop_processed_linear_to_working(const SceneLinearRgbFrame& source, const GeometryPixelRect rect) {
    const std::size_t source_stride = validated_scene_linear_row_stride(source);
    validate_crop_rect(rect, source.dimensions);
    FloatRgbImage output;
    output.dimensions = Dimensions{rect.width, rect.height};
    output.row_stride_bytes = static_cast<std::size_t>(rect.width) * 3U * sizeof(float);
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
    output.samples.resize(checked_interleaved_rgb_sample_count(output.dimensions));
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        const std::size_t source_row = static_cast<std::size_t>(rect.y + y) * source_stride;
        const std::size_t destination_row = static_cast<std::size_t>(y) * rect.width * 3U;
        const std::size_t source_offset = source_row + static_cast<std::size_t>(rect.x) * 3U;
        std::copy_n(
            source.samples.data() + source_offset,
            static_cast<std::size_t>(rect.width) * 3U,
            output.samples.data() + destination_row
        );
    }
    return output;
}

} // namespace

std::size_t checked_interleaved_rgb_sample_count(const Dimensions dimensions) {
    const std::uint64_t pixels = dimensions.pixel_count();
    if (pixels > std::numeric_limits<std::uint64_t>::max() / 3U) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy RGB sample count overflows");
    }
    const std::uint64_t samples = pixels * 3U;
    if (samples > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "proxy RGB buffer exceeds the address space"
        );
    }
    return static_cast<std::size_t>(samples);
}

Dimensions developed_source_dimensions(const DevelopedSourcePixels& source) noexcept {
    return std::visit([](const auto& value) { return value.dimensions; }, source);
}

void validate_developed_source(const SceneLinearRgbFrame& source) {
    static_cast<void>(validated_scene_linear_row_stride(source));
}

void validate_developed_source(const DevelopedSourcePixels& source) {
    std::visit(
        [](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, PixelBuffer>) {
                static_cast<void>(validated_source_row_stride(value));
            } else {
                validate_developed_source(value);
            }
        },
        source
    );
}

FloatRgbImage
resize_developed_source_to_working(const DevelopedSourcePixels& source, const Dimensions target) {
    return std::visit(
        [&](const auto& value) { return resize_processed_linear_to_working(value, target); },
        source
    );
}

FloatRgbImage crop_developed_source_to_working(
    const DevelopedSourcePixels& source,
    const GeometryPixelRect rect
) {
    return std::visit(
        [&](const auto& value) { return crop_processed_linear_to_working(value, rect); },
        source
    );
}

FloatRgbImage
take_scene_linear_region_to_working(SceneLinearRgbFrame region, const Dimensions full_dimensions) {
    const std::size_t row_stride = validated_scene_linear_row_stride(region);
    if (full_dimensions.width == 0U || full_dimensions.height == 0U
        || region.dimensions.width > full_dimensions.width
        || region.dimensions.height > full_dimensions.height
        || row_stride != static_cast<std::size_t>(region.dimensions.width) * 3U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "scene-linear region cannot enter the requested level-zero working space"
        );
    }
    FloatRgbImage output;
    output.dimensions = region.dimensions;
    output.row_stride_bytes = region.row_stride_bytes;
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
    output.level_zero_to_raster_scale_x = 1.0;
    output.level_zero_to_raster_scale_y = 1.0;
    output.samples = std::move(region.samples);
    return output;
}

} // namespace shadow::image::proxy_detail
