#include <shadow/image/edit.hpp>

#include <jpeglib.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
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

void validate_jpeg_quality(const std::uint8_t jpeg_quality) {
    if (jpeg_quality == 0U || jpeg_quality > 100U) {
        throw DecodeError(DecodeErrorCode::invalid_request, 0, "JPEG quality must be in 1..=100");
    }
}

void validate_proxy_request(const ProxyRequest request) {
    constexpr std::uint32_t maximum_proxy_edge = 16'384;
    if (request.max_edge == 0U || request.max_edge > maximum_proxy_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "proxy max edge must be in 1..=16384"
        );
    }
    validate_jpeg_quality(request.jpeg_quality);
}

void validate_warm_edit_max_edge(const std::uint32_t max_edge) {
    if (max_edge == 0U || max_edge > maximum_warm_edit_preview_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "warm edit preview max edge must be in 1..=4096"
        );
    }
}

[[nodiscard]] std::size_t validated_source_row_stride(const PixelBuffer& source) {
    if (
        source.bits_per_channel != 16U || (source.channels != 1U && source.channels != 3U)
        || source.dimensions.width == 0U || source.dimensions.height == 0U
        || source.color_space != ColorSpace::srgb
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "proxy renderer requires non-empty 16-bit grayscale or RGB sRGB input"
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
    return row_stride;
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
    const std::size_t row_stride = validated_source_row_stride(source);

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

[[nodiscard]] double srgb_to_scene_linear(const double encoded) noexcept {
    constexpr double srgb_linear_threshold = 0.04045;
    if (encoded <= srgb_linear_threshold) {
        return encoded / 12.92;
    }
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] std::uint8_t scene_linear_to_srgb8(const float sample) {
    if (!std::isfinite(sample)) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited proxy contains a non-finite scene-linear sample"
        );
    }

    // The edit graph deliberately preserves negative and super-white values. JPEG cannot, so
    // display-range clipping belongs here at the output transform rather than inside a node.
    const double linear = std::clamp(static_cast<double>(sample), 0.0, 1.0);
    constexpr double srgb_linear_threshold = 0.0031308;
    const double encoded = linear <= srgb_linear_threshold
        ? 12.92 * linear
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(encoded * 255.0), 0L, 255L)
    );
}

[[nodiscard]] WorkingRgbSpace linear_srgb_working_space() {
    return WorkingRgbSpace{
        .id = "srgb-d65-linear",
        .primaries = {
            Chromaticity{0.6400, 0.3300},
            Chromaticity{0.3000, 0.6000},
            Chromaticity{0.1500, 0.0600},
        },
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] FloatRgbImage resize_srgb_transfer_to_linear(
    const PixelBuffer& source,
    const Dimensions target
) {
    const std::size_t source_stride = validated_source_row_stride(source);
    const std::size_t sample_count = checked_rgb_size(target);
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
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
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
                    const double encoded = static_cast<double>(
                        source_sample(source, source_stride, x, y, channel)
                    ) / 65'535.0;
                    return srgb_to_scene_linear(encoded);
                };
                const double top = linear_sample(x0, y0) * (1.0 - fraction_x)
                    + linear_sample(x1, y0) * fraction_x;
                const double bottom = linear_sample(x0, y1) * (1.0 - fraction_x)
                    + linear_sample(x1, y1) * fraction_x;
                output.samples[output_index + channel] =
                    static_cast<float>(top * (1.0 - fraction_y) + bottom * fraction_y);
            }
        }
    }
    return output;
}

[[nodiscard]] FloatRgbImage decode_srgb_transfer(const PixelBuffer& source) {
    return resize_srgb_transfer_to_linear(source, source.dimensions);
}

void validate_detail_tile_rect(
    const DetailTileRect rect,
    const Dimensions full_dimensions
) {
    if (
        rect.width == 0U || rect.height == 0U
        || rect.width > maximum_edit_detail_tile_side
        || rect.height > maximum_edit_detail_tile_side
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "detail tile width and height must be in 1..=1024"
        );
    }
    if (
        rect.x >= full_dimensions.width || rect.y >= full_dimensions.height
        || rect.width > full_dimensions.width - rect.x
        || rect.height > full_dimensions.height - rect.y
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "detail tile rectangle must be fully inside the retained image"
        );
    }
}

[[nodiscard]] FloatRgbImage crop_srgb_transfer_to_linear(
    const PixelBuffer& source,
    const DetailTileRect rect
) {
    const std::size_t source_stride = validated_source_row_stride(source);
    const Dimensions tile_dimensions{rect.width, rect.height};
    const std::size_t sample_count = checked_rgb_size(tile_dimensions);
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
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
    output.samples.resize(sample_count);

    for (std::uint32_t output_y = 0; output_y < rect.height; ++output_y) {
        const std::size_t source_y = static_cast<std::size_t>(rect.y) + output_y;
        for (std::uint32_t output_x = 0; output_x < rect.width; ++output_x) {
            const std::size_t source_x = static_cast<std::size_t>(rect.x) + output_x;
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * rect.width + output_x) * 3U;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const double encoded = static_cast<double>(
                    source_sample(source, source_stride, source_x, source_y, channel)
                ) / 65'535.0;
                output.samples[output_index + channel] =
                    static_cast<float>(srgb_to_scene_linear(encoded));
            }
        }
    }
    return output;
}

[[nodiscard]] std::uint64_t checked_detail_retained_bytes(const PixelBuffer& source) {
    if (
        source.samples.capacity()
        > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail retained byte count overflows"
        );
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(source.samples.capacity()) * sizeof(std::uint16_t);
    if (bytes > maximum_full_edit_detail_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail source exceeds the 512 MiB retained limit"
        );
    }
    return bytes;
}

void preflight_detail_metadata(const AssetMetadata& metadata) {
    const std::uint64_t pixels = std::max(
        metadata.raw_dimensions.pixel_count(),
        metadata.image_dimensions.pixel_count()
    );
    constexpr std::uint64_t rgb_u16_bytes_per_pixel = 3U * sizeof(std::uint16_t);
    if (
        pixels == 0U
        || pixels > maximum_full_edit_detail_retained_bytes / rgb_u16_bytes_per_pixel
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail metadata exceeds the 512 MiB worst-case RGB u16 limit"
        );
    }
}

[[nodiscard]] std::vector<std::uint8_t> resize_linear_to_srgb8(
    const FloatRgbImage& source,
    const Dimensions target
) {
    std::vector<std::uint8_t> output(checked_rgb_size(target));
    const std::size_t row_stride = source.row_stride_bytes / sizeof(float);
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
                const auto sample = [&, channel](const std::size_t x, const std::size_t y) {
                    return static_cast<double>(source.samples[(y * row_stride) + (x * 3U) + channel]);
                };
                const double top = sample(x0, y0) * (1.0 - fraction_x)
                    + sample(x1, y0) * fraction_x;
                const double bottom = sample(x0, y1) * (1.0 - fraction_x)
                    + sample(x1, y1) * fraction_x;
                const double linear = top * (1.0 - fraction_y) + bottom * fraction_y;
                output[output_index + channel] = scene_linear_to_srgb8(
                    static_cast<float>(linear)
                );
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

WarmEditPreviewSession::WarmEditPreviewSession(
    FloatRgbImage working_proxy,
    const std::uint32_t max_edge
)
    : working_proxy_(std::move(working_proxy)), max_edge_(max_edge) {}

Dimensions WarmEditPreviewSession::dimensions() const noexcept {
    return working_proxy_.dimensions;
}

std::uint32_t WarmEditPreviewSession::max_edge() const noexcept {
    return max_edge_;
}

EncodedProxy WarmEditPreviewSession::render_jpeg(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality
) const {
    validate_jpeg_quality(jpeg_quality);
    const FloatRgbImage edited = execute_adjustment_nodes(working_proxy_, nodes);
    const auto rgb = resize_linear_to_srgb8(edited, edited.dimensions);

    EncodedProxy proxy;
    proxy.dimensions = edited.dimensions;
    proxy.bytes = encode_jpeg(rgb, edited.dimensions, jpeg_quality);
    return proxy;
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge
) {
    validate_warm_edit_max_edge(max_edge);
    const PixelBuffer reference_rgb = session.render_reference_rgb();
    const Dimensions target = proxy_dimensions(reference_rgb.dimensions, max_edge);
    FloatRgbImage working_proxy = resize_srgb_transfer_to_linear(reference_rgb, target);
    return WarmEditPreviewSession(std::move(working_proxy), max_edge);
}

FullEditDetailSession::FullEditDetailSession(
    PixelBuffer reference_rgb,
    const std::uint64_t retained_bytes
)
    : reference_rgb_(std::move(reference_rgb)), retained_bytes_(retained_bytes) {}

Dimensions FullEditDetailSession::dimensions() const noexcept {
    return reference_rgb_.dimensions;
}

std::uint64_t FullEditDetailSession::retained_bytes() const noexcept {
    return retained_bytes_;
}

RenderedDetailTile FullEditDetailSession::render_rgb8(
    const std::span<const AdjustmentNode> nodes,
    const DetailTileRect rect
) const {
    validate_adjustment_nodes(nodes);
    validate_detail_tile_rect(rect, reference_rgb_.dimensions);
    const FloatRgbImage tile = crop_srgb_transfer_to_linear(reference_rgb_, rect);
    const FloatRgbImage edited = execute_adjustment_nodes(tile, nodes);
    const Dimensions dimensions{rect.width, rect.height};
    auto bytes = resize_linear_to_srgb8(edited, dimensions);
    return RenderedDetailTile{
        .rect = rect,
        .full_dimensions = reference_rgb_.dimensions,
        .row_stride_bytes = rect.width * 3U,
        .bytes = std::move(bytes),
    };
}

FullEditDetailSession prepare_full_edit_detail(const DecodeSession& session) {
    preflight_detail_metadata(session.metadata());
    PixelBuffer reference_rgb = session.render_reference_rgb();
    static_cast<void>(validated_source_row_stride(reference_rgb));
    const std::uint64_t retained_bytes = checked_detail_retained_bytes(reference_rgb);
    return FullEditDetailSession(std::move(reference_rgb), retained_bytes);
}

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
    validate_proxy_request(request);
    const PixelBuffer source = session.render_reference_rgb();
    const Dimensions target = proxy_dimensions(source.dimensions, request.max_edge);
    const auto rgb = resize_to_rgb8(source, target);

    EncodedProxy proxy;
    proxy.dimensions = target;
    proxy.bytes = encode_jpeg(rgb, target, request.jpeg_quality);
    return proxy;
}

EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    const std::span<const AdjustmentNode> nodes,
    const ProxyRequest request
) {
    validate_proxy_request(request);
    validate_adjustment_nodes(nodes);
    const PixelBuffer reference_rgb = session.render_reference_rgb();
    const FloatRgbImage scene_linear = decode_srgb_transfer(reference_rgb);
    const FloatRgbImage edited = execute_adjustment_nodes(scene_linear, nodes);
    const Dimensions target = proxy_dimensions(edited.dimensions, request.max_edge);
    const auto rgb = resize_linear_to_srgb8(edited, target);

    EncodedProxy proxy;
    proxy.dimensions = target;
    proxy.bytes = encode_jpeg(rgb, target, request.jpeg_quality);
    return proxy;
}

} // namespace shadow::image
