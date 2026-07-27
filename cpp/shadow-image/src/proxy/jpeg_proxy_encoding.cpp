#include "jpeg_proxy_encoding.hpp"

#include <shadow/image/decoder_error.hpp>

#include <jpeglib.h>

#include <cstddef>
#include <csetjmp>
#include <cstdlib>
#include <string>
#include <utility>

namespace shadow::image::proxy_detail {

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

} // namespace

void validate_jpeg_quality(const std::uint8_t jpeg_quality) {
    if (jpeg_quality == 0U || jpeg_quality > 100U) {
        throw DecodeError(DecodeErrorCode::invalid_request, 0, "JPEG quality must be in 1..=100");
    }
}

std::optional<std::vector<std::uint8_t>> encode_proxy_jpeg_cancellable(
    const std::span<const std::uint8_t> rgb,
    const Dimensions dimensions,
    const std::uint8_t quality,
    const std::stop_token cancellation
) {
    validate_jpeg_quality(quality);
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
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
    // A warm edit proxy is the user's active grading surface, not a tiny gallery thumbnail.
    // Libjpeg defaults to chroma subsampling, which makes subtle hue and saturation adjustments
    // look blockier than the linear RGB render that produced them. Keep full 4:4:4 chroma until
    // the desktop bridge grows a lossless GPU upload format; thumbnail/cache callers may still
    // choose their own size and quality.
    for (int component = 0; component < encoder.num_components; ++component) {
        encoder.comp_info[component].h_samp_factor = 1;
        encoder.comp_info[component].v_samp_factor = 1;
    }
    jpeg_set_quality(&encoder, quality, TRUE);
    encoder.optimize_coding = TRUE;
    jpeg_start_compress(&encoder, TRUE);

    const std::size_t row_stride = static_cast<std::size_t>(dimensions.width) * 3U;
    while (encoder.next_scanline < encoder.image_height) {
        if (cancellation.stop_requested()) {
            jpeg_abort_compress(&encoder);
            std::free(error.output);
            error.output = nullptr;
            jpeg_destroy_compress(&encoder);
            return std::nullopt;
        }
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
    return cancellation.stop_requested()
        ? std::nullopt
        : std::optional<std::vector<std::uint8_t>>{std::move(result)};
}

std::vector<std::uint8_t> encode_proxy_jpeg(
    const std::span<const std::uint8_t> rgb,
    const Dimensions dimensions,
    const std::uint8_t quality
) {
    auto encoded = encode_proxy_jpeg_cancellable(rgb, dimensions, quality, {});
    if (!encoded.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable JPEG proxy encoding was unexpectedly cancelled"
        );
    }
    return std::move(*encoded);
}

} // namespace shadow::image::proxy_detail
