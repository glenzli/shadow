#include <shadow/image/decoder.hpp>

#include <jpeglib.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace shadow::image {

namespace {

struct JpegErrorManager final {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
    unsigned char* output = nullptr;
    unsigned long output_size = 0;
};

extern "C" void handle_jpeg_error(j_common_ptr context) {
    auto* error = reinterpret_cast<JpegErrorManager*>(context->err);
    (*context->err->format_message)(context, error->message);
    std::longjmp(error->jump, 1);
}

[[nodiscard]] std::size_t checked_rgb_size(const Dimensions dimensions) {
    const std::uint64_t pixels = dimensions.pixel_count();
    if (pixels > std::numeric_limits<std::uint64_t>::max() / 3U) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "proxy RGB sample count overflows"
        );
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

[[nodiscard]] std::uint8_t sample_to_u8(const std::uint16_t value) noexcept {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) + 128U) / 257U);
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

[[nodiscard]] std::vector<std::uint8_t> resize_to_rgb8(
    const PixelBuffer& source,
    const Dimensions target
) {
    if (
        source.bits_per_channel != 16U || (source.channels != 1U && source.channels != 3U)
        || source.dimensions.width == 0U || source.dimensions.height == 0U
        || source.color_space != ColorSpace::srgb
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "proxy renderer requires non-empty 16-bit grayscale or RGB input"
        );
    }
    if (
        static_cast<std::uint64_t>(source.dimensions.width) * source.channels
        > std::numeric_limits<std::size_t>::max()
    ) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy source stride overflows");
    }
    const std::size_t minimum_stride =
        static_cast<std::size_t>(source.dimensions.width) * source.channels;
    if (
        source.row_stride_bytes % sizeof(std::uint16_t) != 0U
        || source.row_stride_bytes / sizeof(std::uint16_t) < minimum_stride
    ) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "proxy source row stride is invalid");
    }
    const std::size_t row_stride = source.row_stride_bytes / sizeof(std::uint16_t);
    if (
        static_cast<std::uint64_t>(row_stride) * source.dimensions.height
        > std::numeric_limits<std::size_t>::max()
    ) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy source buffer overflows");
    }
    const std::size_t required_samples =
        row_stride * static_cast<std::size_t>(source.dimensions.height);
    if (source.samples.size() < required_samples) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "proxy source buffer is truncated");
    }

    std::vector<std::uint8_t> output(checked_rgb_size(target));
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
                const double top =
                    static_cast<double>(source_sample(source, row_stride, x0, y0, channel))
                        * (1.0 - fraction_x)
                    + static_cast<double>(source_sample(source, row_stride, x1, y0, channel))
                        * fraction_x;
                const double bottom =
                    static_cast<double>(source_sample(source, row_stride, x0, y1, channel))
                        * (1.0 - fraction_x)
                    + static_cast<double>(source_sample(source, row_stride, x1, y1, channel))
                        * fraction_x;
                const auto value = static_cast<std::uint16_t>(
                    std::clamp(std::lround(top * (1.0 - fraction_y) + bottom * fraction_y), 0L, 65'535L)
                );
                output[output_index + channel] = sample_to_u8(value);
            }
        }
    }
    return output;
}

[[nodiscard]] std::vector<std::uint8_t> encode_jpeg(
    const std::vector<std::uint8_t>& rgb,
    const Dimensions dimensions,
    const std::uint8_t quality
) {
    jpeg_compress_struct encoder{};
    JpegErrorManager error{};
    encoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = handle_jpeg_error;
    if (setjmp(error.jump) != 0) {
        std::free(error.output);
        jpeg_destroy_compress(&encoder);
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            std::string("JPEG proxy encoding failed: ") + error.message
        );
    }

    jpeg_create_compress(&encoder);
    jpeg_mem_dest(&encoder, &error.output, &error.output_size);
    encoder.image_width = dimensions.width;
    encoder.image_height = dimensions.height;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, quality, TRUE);
    encoder.optimize_coding = TRUE;
    jpeg_start_compress(&encoder, TRUE);

    const std::size_t row_stride = static_cast<std::size_t>(dimensions.width) * 3U;
    while (encoder.next_scanline < encoder.image_height) {
        auto* row = const_cast<JSAMPLE*>(
            rgb.data() + static_cast<std::size_t>(encoder.next_scanline) * row_stride
        );
        JSAMPROW rows[] = {row};
        jpeg_write_scanlines(&encoder, rows, 1);
    }
    jpeg_finish_compress(&encoder);

    std::vector<std::uint8_t> result(error.output, error.output + error.output_size);
    std::free(error.output);
    error.output = nullptr;
    jpeg_destroy_compress(&encoder);
    return result;
}

} // namespace

Dimensions proxy_dimensions(const Dimensions source, const std::uint32_t max_edge) {
    if (source.width == 0U || source.height == 0U || max_edge == 0U) {
        throw DecodeError(DecodeErrorCode::invalid_request, 0, "proxy dimensions must be non-zero");
    }
    const std::uint32_t source_edge = std::max(source.width, source.height);
    if (source_edge <= max_edge) {
        return source;
    }
    const auto scaled = [source_edge, max_edge](const std::uint32_t value) {
        const std::uint64_t numerator = static_cast<std::uint64_t>(value) * max_edge;
        return std::max(1U, static_cast<std::uint32_t>((numerator + source_edge / 2U) / source_edge));
    };
    return Dimensions{scaled(source.width), scaled(source.height)};
}

EncodedProxy render_reference_proxy_jpeg(const DecodeSession& session, const ProxyRequest request) {
    constexpr std::uint32_t maximum_proxy_edge = 16'384;
    if (request.max_edge == 0U || request.max_edge > maximum_proxy_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "proxy max edge must be in 1..=16384"
        );
    }
    if (request.jpeg_quality == 0U || request.jpeg_quality > 100U) {
        throw DecodeError(DecodeErrorCode::invalid_request, 0, "JPEG quality must be in 1..=100");
    }
    const PixelBuffer source = session.render_reference_rgb();
    const Dimensions target = proxy_dimensions(source.dimensions, request.max_edge);
    const auto rgb = resize_to_rgb8(source, target);

    EncodedProxy proxy;
    proxy.dimensions = target;
    proxy.bytes = encode_jpeg(rgb, target, request.jpeg_quality);
    return proxy;
}

} // namespace shadow::image
