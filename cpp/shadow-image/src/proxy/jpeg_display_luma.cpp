#include <shadow/image/display_luma.hpp>

#include "display_rgb_math.hpp"

#include <jconfig.h>
#include <jpeglib.h>
#include <jerror.h>

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

#if !defined(LIBJPEG_TURBO_VERSION_NUMBER)
#error "Shadow display-luma preprocessing requires libjpeg-turbo"
#endif

#if !defined(MEM_SRCDST_SUPPORTED) || !MEM_SRCDST_SUPPORTED
#error "Shadow display-luma preprocessing requires jpeg_mem_src support"
#endif

#if !defined(BITS_IN_JSAMPLE) || BITS_IN_JSAMPLE != 8
#error "Shadow display-luma preprocessing requires 8-bit JSAMPLE values"
#endif

#define SHADOW_STRINGIFY_IMPL(value) #value
#define SHADOW_STRINGIFY(value) SHADOW_STRINGIFY_IMPL(value)

inline constexpr std::string_view preprocessing_version_prefix =
    "shadow.jpeg-luma.v2:libjpeg-turbo-" SHADOW_STRINGIFY(LIBJPEG_TURBO_VERSION)
    ":rgb8:islow:no-fancy-upsampling:no-block-smoothing:assume-srgb:ignore-icc:"
    "stored-orientation:idct-scale-1-2-4-8:bilinear-center-q16:rec709-encoded-q16";

#undef SHADOW_STRINGIFY
#undef SHADOW_STRINGIFY_IMPL

inline constexpr std::uint32_t maximum_scaled_decode_edge = 8'192U;
inline constexpr std::uint64_t maximum_scaled_decode_pixels = 8'388'608U;
inline constexpr std::uint64_t maximum_multiscan_source_pixels = 50'000'000U;
inline constexpr long maximum_libjpeg_memory_bytes = 256L * 1024L * 1024L;
static_assert(sizeof(JSAMPLE) == 1U);
static_assert(MAXJSAMPLE == 255);

enum class RawDecodeStatus : std::uint8_t {
    success,
    corrupt_data,
    unsupported_layout,
    resource_limit,
    internal,
};

struct RawDecodedLuma final {
    RawDecodeStatus status = RawDecodeStatus::internal;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t scaled_width = 0U;
    std::uint32_t scaled_height = 0U;
    std::uint32_t* weighted_samples = nullptr;
    std::size_t sample_count = 0U;
    char message[256]{};
};

struct JpegErrorManager final {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    int fatal_code = JMSG_NOMESSAGE;
    char fatal_message[JMSG_LENGTH_MAX]{};
    char first_warning[JMSG_LENGTH_MAX]{};
};

struct JpegDecodeState final {
    jpeg_decompress_struct decoder{};
    JpegErrorManager error{};
    bool decoder_created = false;
    JSAMPLE* scanline = nullptr;
    std::uint32_t* weighted_luma = nullptr;
    std::size_t weighted_luma_count = 0U;
};

extern "C" void handle_jpeg_fatal_error(j_common_ptr context) {
    auto* error = reinterpret_cast<JpegErrorManager*>(context->err);
    error->fatal_code = context->err->msg_code;
    (*context->err->format_message)(context, error->fatal_message);
    std::longjmp(error->jump, 1);
}

extern "C" void handle_jpeg_message(j_common_ptr context, const int message_level) {
    if (message_level >= 0) {
        return;
    }

    auto* error = reinterpret_cast<JpegErrorManager*>(context->err);
    ++error->base.num_warnings;
    if (error->first_warning[0] == '\0') {
        (*context->err->format_message)(context, error->first_warning);
    }
}

void copy_message(char (&destination)[256], const char* message) noexcept {
    const char* source = message == nullptr || message[0] == '\0'
        ? "unknown JPEG decoding error"
        : message;
    static_cast<void>(std::snprintf(destination, sizeof(destination), "%s", source));
}

[[nodiscard]] RawDecodeStatus classify_jpeg_fatal_error(const int code) noexcept {
    switch (code) {
    case JERR_BAD_ALLOC_CHUNK:
    case JERR_BUFFER_SIZE:
    case JERR_IMAGE_TOO_BIG:
    case JERR_NO_BACKING_STORE:
    case JERR_OUT_OF_MEMORY:
    case JERR_WIDTH_OVERFLOW:
        return RawDecodeStatus::resource_limit;
#if JPEG_LIB_VERSION < 70
    case JERR_ARITH_NOTIMPL:
#endif
    case JERR_BAD_DCTSIZE:
    case JERR_BAD_PRECISION:
    case JERR_CCIR601_NOTIMPL:
    case JERR_CONVERSION_NOTIMPL:
    case JERR_FRACT_SAMPLE_NOTIMPL:
    case JERR_NOT_COMPILED:
    case JERR_NOTIMPL:
    case JERR_SOF_UNSUPPORTED:
        return RawDecodeStatus::unsupported_layout;
    default:
        return RawDecodeStatus::corrupt_data;
    }
}

void cleanup_decode_state(JpegDecodeState& state, const bool keep_luma) noexcept {
    std::free(state.scanline);
    state.scanline = nullptr;
    if (!keep_luma) {
        std::free(state.weighted_luma);
        state.weighted_luma = nullptr;
        state.weighted_luma_count = 0U;
    }
    if (state.decoder_created) {
        jpeg_destroy_decompress(&state.decoder);
        state.decoder_created = false;
    }
}

[[nodiscard]] Dimensions bounded_dimensions(
    const Dimensions source,
    const std::uint32_t max_edge
) noexcept {
    const std::uint32_t source_edge = std::max(source.width, source.height);
    if (source_edge <= max_edge) {
        return source;
    }
    const auto scaled = [source_edge, max_edge](const std::uint32_t value) {
        const std::uint64_t numerator = static_cast<std::uint64_t>(value) * max_edge;
        return std::max(
            1U,
            static_cast<std::uint32_t>((numerator + source_edge / 2U) / source_edge)
        );
    };
    return Dimensions{scaled(source.width), scaled(source.height)};
}

[[nodiscard]] bool source_is_within_limits(
    const std::uint32_t width,
    const std::uint32_t height
) noexcept {
    if (
        width == 0U || height == 0U
        || width > maximum_jpeg_display_luma_source_dimension
        || height > maximum_jpeg_display_luma_source_dimension
    ) {
        return false;
    }
    return static_cast<std::uint64_t>(width) * height
        <= maximum_jpeg_display_luma_source_pixels;
}

[[nodiscard]] bool scaled_output_is_within_limits(
    const JDIMENSION width,
    const JDIMENSION height
) noexcept {
    return width != 0U && height != 0U
        && width <= maximum_scaled_decode_edge && height <= maximum_scaled_decode_edge
        && static_cast<std::uint64_t>(width) * height <= maximum_scaled_decode_pixels;
}

void select_idct_scale(
    jpeg_decompress_struct& decoder,
    const Dimensions target
) {
    constexpr std::array<unsigned int, 4> denominators{8U, 4U, 2U, 1U};
    for (const unsigned int denominator : denominators) {
        decoder.scale_num = 1U;
        decoder.scale_denom = denominator;
        jpeg_calc_output_dimensions(&decoder);
        if (decoder.output_width >= target.width && decoder.output_height >= target.height) {
            return;
        }
    }
    decoder.scale_num = 1U;
    decoder.scale_denom = 1U;
    jpeg_calc_output_dimensions(&decoder);
}

[[nodiscard]] RawDecodedLuma raw_decode_jpeg_luma(
    const std::uint8_t* const encoded,
    const std::size_t encoded_size,
    const std::uint32_t max_edge
) noexcept {
    RawDecodedLuma result;
    // libjpeg reports fatal errors with longjmp. Mutable cleanup state therefore
    // lives on the heap: C/C++ make automatic locals changed after setjmp
    // indeterminate when control returns through longjmp.
    auto* const state = new (std::nothrow) JpegDecodeState{};
    if (state == nullptr) {
        result.status = RawDecodeStatus::resource_limit;
        copy_message(result.message, "JPEG decoder state allocation failed");
        return result;
    }
    state->decoder.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = handle_jpeg_fatal_error;
    state->error.base.emit_message = handle_jpeg_message;

    if (setjmp(state->error.jump) != 0) {
        RawDecodedLuma failure;
        failure.status = classify_jpeg_fatal_error(state->error.fatal_code);
        copy_message(failure.message, state->error.fatal_message);
        cleanup_decode_state(*state, false);
        delete state;
        return failure;
    }

    // Mark the zero-initialized structure for cleanup before creation so a
    // libjpeg allocation failure cannot leak a partially created pool.
    state->decoder_created = true;
    jpeg_create_decompress(&state->decoder);
    state->decoder.mem->max_memory_to_use = maximum_libjpeg_memory_bytes;
    jpeg_mem_src(
        &state->decoder,
        const_cast<unsigned char*>(encoded),
        static_cast<unsigned long>(encoded_size)
    );
    static_cast<void>(jpeg_read_header(&state->decoder, TRUE));

    result.source_width = state->decoder.image_width;
    result.source_height = state->decoder.image_height;
    if (!source_is_within_limits(result.source_width, result.source_height)) {
        result.status = RawDecodeStatus::resource_limit;
        copy_message(result.message, "JPEG source dimensions or pixel count exceed limits");
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }
    const std::uint64_t source_pixels =
        static_cast<std::uint64_t>(result.source_width) * result.source_height;
    if (
        jpeg_has_multiple_scans(&state->decoder) == TRUE
        && source_pixels > maximum_multiscan_source_pixels
    ) {
        result.status = RawDecodeStatus::resource_limit;
        copy_message(
            result.message,
            "multi-scan JPEG source pixels exceed the bounded decoder limit"
        );
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }

    const Dimensions target = bounded_dimensions(
        Dimensions{result.source_width, result.source_height},
        max_edge
    );
    state->decoder.out_color_space = JCS_RGB;
    state->decoder.dct_method = JDCT_ISLOW;
    state->decoder.do_fancy_upsampling = FALSE;
    state->decoder.do_block_smoothing = FALSE;
    select_idct_scale(state->decoder, target);
    if (!scaled_output_is_within_limits(
            state->decoder.output_width,
            state->decoder.output_height
        )) {
        result.status = RawDecodeStatus::resource_limit;
        copy_message(result.message, "scaled JPEG decode dimensions exceed limits");
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }

    static_cast<void>(jpeg_start_decompress(&state->decoder));
    if (state->decoder.output_components != 3) {
        result.status = RawDecodeStatus::unsupported_layout;
        copy_message(result.message, "JPEG decoder did not produce RGB8 samples");
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }

    result.scaled_width = state->decoder.output_width;
    result.scaled_height = state->decoder.output_height;
    const std::uint64_t row_bytes_u64 = static_cast<std::uint64_t>(result.scaled_width) * 3U;
    const std::uint64_t sample_count_u64 =
        static_cast<std::uint64_t>(result.scaled_width) * result.scaled_height;
    if (
        row_bytes_u64 > std::numeric_limits<std::size_t>::max()
        || sample_count_u64 > std::numeric_limits<std::size_t>::max()
        || sample_count_u64
            > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)
    ) {
        result.status = RawDecodeStatus::resource_limit;
        copy_message(result.message, "JPEG display-luma allocation size overflows");
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(row_bytes_u64);
    state->weighted_luma_count = static_cast<std::size_t>(sample_count_u64);
    state->scanline = static_cast<JSAMPLE*>(std::malloc(row_bytes));
    state->weighted_luma = static_cast<std::uint32_t*>(
        std::malloc(state->weighted_luma_count * sizeof(std::uint32_t))
    );
    if (state->scanline == nullptr || state->weighted_luma == nullptr) {
        result.status = RawDecodeStatus::resource_limit;
        copy_message(result.message, "JPEG display-luma allocation failed");
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }

    while (state->decoder.output_scanline < state->decoder.output_height) {
        JSAMPROW rows[] = {state->scanline};
        if (jpeg_read_scanlines(&state->decoder, rows, 1U) != 1U) {
            result.status = RawDecodeStatus::corrupt_data;
            copy_message(result.message, "JPEG scanline decoding stopped early");
            cleanup_decode_state(*state, false);
            delete state;
            return result;
        }

        const std::size_t row = static_cast<std::size_t>(state->decoder.output_scanline - 1U);
        std::uint32_t* const output =
            state->weighted_luma + row * result.scaled_width;
        for (std::size_t column = 0U; column < result.scaled_width; ++column) {
            const std::size_t source = column * 3U;
            output[column] = display_rgb::rec709_encoded_luma_q16(
                state->scanline[source],
                state->scanline[source + 1U],
                state->scanline[source + 2U]
            );
        }
    }

    static_cast<void>(jpeg_finish_decompress(&state->decoder));
    if (state->error.base.num_warnings != 0) {
        result.status = RawDecodeStatus::corrupt_data;
        copy_message(result.message, state->error.first_warning);
        cleanup_decode_state(*state, false);
        delete state;
        return result;
    }

    result.status = RawDecodeStatus::success;
    result.weighted_samples = state->weighted_luma;
    result.sample_count = state->weighted_luma_count;
    state->weighted_luma = nullptr;
    state->weighted_luma_count = 0U;
    cleanup_decode_state(*state, true);
    delete state;
    return result;
}

[[noreturn]] void throw_raw_decode_error(const RawDecodedLuma& decoded) {
    DecodeErrorCode code = DecodeErrorCode::internal;
    switch (decoded.status) {
    case RawDecodeStatus::corrupt_data:
        code = DecodeErrorCode::corrupt_data;
        break;
    case RawDecodeStatus::unsupported_layout:
        code = DecodeErrorCode::unsupported_layout;
        break;
    case RawDecodeStatus::resource_limit:
        code = DecodeErrorCode::resource_limit;
        break;
    case RawDecodeStatus::success:
    case RawDecodeStatus::internal:
        code = DecodeErrorCode::internal;
        break;
    }
    throw DecodeError(
        code,
        0,
        std::string("JPEG display-luma decoding failed: ") + decoded.message
    );
}

struct AxisCoordinate final {
    std::uint32_t first = 0U;
    std::uint32_t second = 0U;
    std::uint64_t second_weight = 0U;
    std::uint64_t denominator = 1U;
};

[[nodiscard]] AxisCoordinate source_coordinate(
    const std::uint32_t output_index,
    const std::uint32_t source_size,
    const std::uint32_t target_size
) noexcept {
    const std::uint64_t denominator = static_cast<std::uint64_t>(target_size) * 2U;
    const std::uint64_t centered_source =
        (static_cast<std::uint64_t>(output_index) * 2U + 1U) * source_size;
    if (centered_source <= target_size) {
        return AxisCoordinate{0U, 0U, 0U, denominator};
    }

    const std::uint64_t numerator = centered_source - target_size;
    const auto first = static_cast<std::uint32_t>(numerator / denominator);
    if (first >= source_size - 1U) {
        return AxisCoordinate{source_size - 1U, source_size - 1U, 0U, denominator};
    }
    return AxisCoordinate{
        first,
        first + 1U,
        numerator % denominator,
        denominator,
    };
}

[[nodiscard]] std::uint32_t interpolate_luma(
    const std::uint32_t* const source,
    const std::uint32_t source_width,
    const AxisCoordinate x,
    const AxisCoordinate y
) noexcept {
    const auto sample = [source, source_width](
                            const std::uint32_t column,
                            const std::uint32_t row
                        ) {
        return source[static_cast<std::size_t>(row) * source_width + column];
    };
    const std::uint64_t first_x_weight = x.denominator - x.second_weight;
    const std::uint64_t first_y_weight = y.denominator - y.second_weight;
    const std::uint64_t top = static_cast<std::uint64_t>(sample(x.first, y.first))
            * first_x_weight
        + static_cast<std::uint64_t>(sample(x.second, y.first)) * x.second_weight;
    const std::uint64_t bottom = static_cast<std::uint64_t>(sample(x.first, y.second))
            * first_x_weight
        + static_cast<std::uint64_t>(sample(x.second, y.second)) * x.second_weight;
    const std::uint64_t denominator = x.denominator * y.denominator;
    const std::uint64_t weighted =
        top * first_y_weight + bottom * y.second_weight;
    return static_cast<std::uint32_t>((weighted + denominator / 2U) / denominator);
}

[[nodiscard]] std::vector<float> resize_normalized_luma(
    const RawDecodedLuma& decoded,
    const Dimensions target
) {
    const std::uint64_t target_count = target.pixel_count();
    if (target_count > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "JPEG display-luma output size exceeds the address space"
        );
    }

    std::vector<float> output(static_cast<std::size_t>(target_count));
    for (std::uint32_t row = 0U; row < target.height; ++row) {
        const AxisCoordinate y = source_coordinate(row, decoded.scaled_height, target.height);
        for (std::uint32_t column = 0U; column < target.width; ++column) {
            const AxisCoordinate x =
                source_coordinate(column, decoded.scaled_width, target.width);
            const std::uint32_t weighted = interpolate_luma(
                decoded.weighted_samples,
                decoded.scaled_width,
                x,
                y
            );
            output[static_cast<std::size_t>(row) * target.width + column] =
                static_cast<float>(weighted)
                / static_cast<float>(display_rgb::maximum_weighted_luma);
        }
    }
    return output;
}

} // namespace

std::string_view jpeg_display_luma_preprocessing_version_prefix() noexcept {
    return preprocessing_version_prefix;
}

DisplayLumaImage decode_jpeg_display_luma(
    const std::span<const std::uint8_t> encoded,
    const std::uint32_t max_edge
) {
    if (max_edge == 0U || max_edge > jpeg_display_luma_max_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "JPEG display-luma max edge must be in 1..=512"
        );
    }
    if (encoded.empty()) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "JPEG display-luma input is empty"
        );
    }
    if (encoded.size() > maximum_jpeg_display_luma_encoded_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "JPEG display-luma encoded input exceeds 128 MiB"
        );
    }
    if (encoded.size() > std::numeric_limits<unsigned long>::max()) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "JPEG display-luma encoded input does not fit libjpeg's size type"
        );
    }

    RawDecodedLuma decoded = raw_decode_jpeg_luma(encoded.data(), encoded.size(), max_edge);
    if (decoded.status != RawDecodeStatus::success) {
        throw_raw_decode_error(decoded);
    }

    struct WeightedLumaOwner final {
        std::uint32_t* samples;
        ~WeightedLumaOwner() {
            std::free(samples);
        }
    } owner{decoded.weighted_samples};

    const Dimensions target = bounded_dimensions(
        Dimensions{decoded.source_width, decoded.source_height},
        max_edge
    );
    DisplayLumaImage result;
    result.dimensions = target;
    result.row_stride_samples = target.width;
    result.samples = resize_normalized_luma(decoded, target);
    result.preprocessing_version = std::string(jpeg_display_luma_preprocessing_version_prefix())
        + ":max-edge-" + std::to_string(max_edge);
    return result;
}

} // namespace shadow::image
