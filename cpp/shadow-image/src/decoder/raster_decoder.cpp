#include <shadow/image/color_management.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/decoder_metadata.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>
#include <shadow/image/reference_pixels.hpp>

#include "heif_decoder.hpp"
#include "raster_exif.hpp"

#include <jconfig.h>
#include <jerror.h>
#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

#if !defined(LIBJPEG_TURBO_VERSION_NUMBER)
#error "Shadow's JPEG raster provider requires libjpeg-turbo"
#endif

#if !defined(BITS_IN_JSAMPLE) || BITS_IN_JSAMPLE != 8
#error "Shadow's JPEG raster provider requires 8-bit JSAMPLE values"
#endif

inline constexpr std::uint32_t raster_decoder_contract_version = 1U;
inline constexpr std::uint64_t maximum_raster_source_pixels = 50'000'000U;
inline constexpr std::uint32_t maximum_raster_source_dimension = 65'535U;
inline constexpr std::uint64_t maximum_embedded_icc_bytes = 16U * 1024U * 1024U;
inline constexpr std::uint32_t maximum_preview_edge = 4'096U;

#define SHADOW_STRINGIFY_IMPL(value) #value
#define SHADOW_STRINGIFY(value) SHADOW_STRINGIFY_IMPL(value)

struct JpegErrorManager final {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};

struct JpegDecodeState final {
    jpeg_decompress_struct decoder{};
    JpegErrorManager error{};
    bool decoder_created = false;
    std::FILE* file = nullptr;
    std::uint8_t* rgb = nullptr;
    std::size_t rgb_bytes = 0U;
    std::uint8_t* icc = nullptr;
    std::size_t icc_bytes = 0U;
};

struct DecodedJpeg final {
    Dimensions stored_dimensions;
    Dimensions decoded_dimensions;
    RasterExif exif;
    std::vector<std::uint8_t> rgb;
    std::vector<std::uint8_t> icc;
};

extern "C" void handle_raster_jpeg_error(j_common_ptr context) {
    auto* error = reinterpret_cast<JpegErrorManager*>(context->err);
    (*context->err->format_message)(context, error->message);
    std::longjmp(error->jump, 1);
}

void cleanup(JpegDecodeState& state) noexcept {
    std::free(state.rgb);
    state.rgb = nullptr;
    state.rgb_bytes = 0U;
    std::free(state.icc);
    state.icc = nullptr;
    state.icc_bytes = 0U;
    if (state.decoder_created) {
        jpeg_destroy_decompress(&state.decoder);
        state.decoder_created = false;
    }
    if (state.file != nullptr) {
        static_cast<void>(std::fclose(state.file));
        state.file = nullptr;
    }
}

[[nodiscard]] std::FILE* open_binary_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

[[nodiscard]] std::string path_for_message(const std::filesystem::path& path) {
    const auto native = path.u8string();
    return native.empty()
        ? std::string{"<unnamed file>"}
        : std::string(native.begin(), native.end());
}

[[nodiscard]] bool dimensions_within_limits(const Dimensions dimensions) noexcept {
    return dimensions.width > 0U && dimensions.height > 0U
        && dimensions.width <= maximum_raster_source_dimension
        && dimensions.height <= maximum_raster_source_dimension
        && dimensions.pixel_count() <= maximum_raster_source_pixels;
}

[[nodiscard]] Dimensions oriented_dimensions(
    const Dimensions stored,
    const std::uint16_t orientation
) noexcept {
    switch (orientation) {
    case 5U:
    case 6U:
    case 7U:
    case 8U:
        return Dimensions{stored.height, stored.width};
    default:
        return stored;
    }
}

void parse_jpeg_exif(const jpeg_saved_marker_ptr markers, RasterExif& exif) {
    for (auto marker = markers; marker != nullptr; marker = marker->next) {
        constexpr std::array<std::uint8_t, 6U> exif_prefix{
            static_cast<std::uint8_t>('E'), static_cast<std::uint8_t>('x'),
            static_cast<std::uint8_t>('i'), static_cast<std::uint8_t>('f'), 0U, 0U,
        };
        if (marker->marker != JPEG_APP0 + 1U || marker->data_length <= exif_prefix.size()
            || std::memcmp(marker->data, exif_prefix.data(), exif_prefix.size()) != 0) {
            continue;
        }
        parse_tiff_exif(
            std::span<const std::uint8_t>(
                marker->data + exif_prefix.size(),
                static_cast<std::size_t>(marker->data_length) - exif_prefix.size()
            ),
            exif
        );
        return;
    }
}

void copy_embedded_icc(jpeg_decompress_struct& decoder, JpegDecodeState& state) {
    constexpr std::array<std::uint8_t, 12U> signature{
        static_cast<std::uint8_t>('I'), static_cast<std::uint8_t>('C'),
        static_cast<std::uint8_t>('C'), static_cast<std::uint8_t>('_'),
        static_cast<std::uint8_t>('P'), static_cast<std::uint8_t>('R'),
        static_cast<std::uint8_t>('O'), static_cast<std::uint8_t>('F'),
        static_cast<std::uint8_t>('I'), static_cast<std::uint8_t>('L'),
        static_cast<std::uint8_t>('E'), 0U,
    };
    struct Segment final {
        const JOCTET* bytes = nullptr;
        std::size_t count = 0U;
    };
    std::array<Segment, 255U> segments{};
    std::uint8_t expected_count = 0U;
    bool saw_icc = false;
    for (auto marker = decoder.marker_list; marker != nullptr; marker = marker->next) {
        if (marker->marker != JPEG_APP0 + 2U || marker->data_length < signature.size() + 2U
            || std::memcmp(marker->data, signature.data(), signature.size()) != 0) {
            continue;
        }
        saw_icc = true;
        const std::uint8_t sequence = marker->data[signature.size()];
        const std::uint8_t count = marker->data[signature.size() + 1U];
        if (sequence == 0U || count == 0U || sequence > count
            || (expected_count != 0U && expected_count != count)
            || segments[sequence - 1U].bytes != nullptr) {
            throw DecodeError(
                DecodeErrorCode::corrupt_data,
                0,
                "JPEG embedded ICC chunks are malformed"
            );
        }
        expected_count = count;
        segments[sequence - 1U] = Segment{
            marker->data + signature.size() + 2U,
            static_cast<std::size_t>(marker->data_length) - signature.size() - 2U,
        };
    }
    if (!saw_icc) {
        return;
    }
    std::size_t total_bytes = 0U;
    for (std::uint8_t index = 0U; index < expected_count; ++index) {
        if (segments[index].bytes == nullptr
            || segments[index].count > maximum_embedded_icc_bytes - total_bytes) {
            throw DecodeError(
                DecodeErrorCode::corrupt_data,
                0,
                "JPEG embedded ICC chunks are incomplete or exceed the supported size"
            );
        }
        total_bytes += segments[index].count;
    }
    state.icc = static_cast<std::uint8_t*>(std::malloc(total_bytes));
    if (state.icc == nullptr) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "JPEG embedded ICC allocation failed"
        );
    }
    state.icc_bytes = total_bytes;
    std::size_t offset = 0U;
    for (std::uint8_t index = 0U; index < expected_count; ++index) {
        std::memcpy(state.icc + offset, segments[index].bytes, segments[index].count);
        offset += segments[index].count;
    }
}

void select_idct_scale(jpeg_decompress_struct& decoder, const std::uint32_t max_edge) {
    if (max_edge == 0U) {
        return;
    }
    constexpr std::array<unsigned int, 4U> denominators{8U, 4U, 2U, 1U};
    for (const unsigned int denominator : denominators) {
        decoder.scale_num = 1U;
        decoder.scale_denom = denominator;
        jpeg_calc_output_dimensions(&decoder);
        if (std::max(decoder.output_width, decoder.output_height) >= max_edge) {
            return;
        }
    }
    decoder.scale_num = 1U;
    decoder.scale_denom = 1U;
    jpeg_calc_output_dimensions(&decoder);
}

[[nodiscard]] DecodedJpeg decode_jpeg_file(
    const std::filesystem::path& path,
    const bool decode_pixels,
    const std::uint32_t max_edge = 0U
) {
    auto* const state = new JpegDecodeState{};
    state->decoder.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = handle_raster_jpeg_error;
    if (setjmp(state->error.jump) != 0) {
        const std::string message = state->error.message[0] == '\0'
            ? "unknown JPEG decoder error"
            : std::string(state->error.message);
        cleanup(*state);
        delete state;
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "JPEG decode failed: " + message);
    }

    try {
        state->file = open_binary_file(path);
        if (state->file == nullptr) {
            const int error = errno;
            throw DecodeError(
                DecodeErrorCode::io,
                error,
                "cannot open JPEG " + path_for_message(path) + ": " + std::strerror(error)
            );
        }
        state->decoder_created = true;
        jpeg_create_decompress(&state->decoder);
        jpeg_save_markers(&state->decoder, JPEG_APP0 + 1, 0xffffU);
        jpeg_save_markers(&state->decoder, JPEG_APP0 + 2, 0xffffU);
        jpeg_stdio_src(&state->decoder, state->file);
        if (jpeg_read_header(&state->decoder, TRUE) != JPEG_HEADER_OK) {
            throw DecodeError(DecodeErrorCode::corrupt_data, 0, "JPEG header is incomplete");
        }
        if (state->decoder.data_precision != 8) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "JPEG provider currently supports 8-bit source samples only"
            );
        }
        const Dimensions stored{
            static_cast<std::uint32_t>(state->decoder.image_width),
            static_cast<std::uint32_t>(state->decoder.image_height),
        };
        if (!dimensions_within_limits(stored)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "JPEG dimensions exceed Shadow's retained raster limits"
            );
        }
        RasterExif exif;
        parse_jpeg_exif(state->decoder.marker_list, exif);
        copy_embedded_icc(state->decoder, *state);

        if (!decode_pixels) {
            DecodedJpeg result;
            result.stored_dimensions = stored;
            result.decoded_dimensions = stored;
            result.exif = exif;
            if (state->icc_bytes != 0U) {
                result.icc.assign(state->icc, state->icc + state->icc_bytes);
            }
            cleanup(*state);
            delete state;
            return result;
        }

        state->decoder.out_color_space = JCS_RGB;
        select_idct_scale(state->decoder, std::min(max_edge, maximum_preview_edge));
        if (jpeg_start_decompress(&state->decoder) == FALSE
            || state->decoder.output_components != 3U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "JPEG provider could not produce interleaved RGB samples"
            );
        }
        const Dimensions decoded{
            static_cast<std::uint32_t>(state->decoder.output_width),
            static_cast<std::uint32_t>(state->decoder.output_height),
        };
        if (!dimensions_within_limits(decoded)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "JPEG decoded dimensions exceed Shadow's retained raster limits"
            );
        }
        const std::uint64_t bytes = decoded.pixel_count() * 3U;
        if (bytes > std::numeric_limits<std::size_t>::max()) {
            throw DecodeError(DecodeErrorCode::resource_limit, 0, "JPEG RGB allocation overflows");
        }
        state->rgb = static_cast<std::uint8_t*>(std::malloc(static_cast<std::size_t>(bytes)));
        if (state->rgb == nullptr) {
            throw DecodeError(DecodeErrorCode::resource_limit, 0, "JPEG RGB allocation failed");
        }
        state->rgb_bytes = static_cast<std::size_t>(bytes);
        const std::size_t row_stride = static_cast<std::size_t>(decoded.width) * 3U;
        while (state->decoder.output_scanline < state->decoder.output_height) {
            auto* row = reinterpret_cast<JSAMPLE*>(
                state->rgb + static_cast<std::size_t>(state->decoder.output_scanline) * row_stride
            );
            JSAMPROW rows[]{row};
            if (jpeg_read_scanlines(&state->decoder, rows, static_cast<JDIMENSION>(1))
                != static_cast<JDIMENSION>(1)) {
                throw DecodeError(DecodeErrorCode::corrupt_data, 0, "JPEG scanline decode failed");
            }
        }
        if (jpeg_finish_decompress(&state->decoder) == FALSE) {
            throw DecodeError(DecodeErrorCode::corrupt_data, 0, "JPEG stream did not finish");
        }

        DecodedJpeg result;
        result.stored_dimensions = stored;
        result.decoded_dimensions = decoded;
        result.exif = exif;
        result.rgb.assign(state->rgb, state->rgb + state->rgb_bytes);
        if (state->icc_bytes != 0U) {
            result.icc.assign(state->icc, state->icc + state->icc_bytes);
        }
        cleanup(*state);
        delete state;
        return result;
    } catch (...) {
        cleanup(*state);
        delete state;
        throw;
    }
}

[[nodiscard]] std::string clean_exif_string(const char* value) {
    if (value == nullptr) {
        return {};
    }
    std::string result(value);
    const auto first = result.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const auto last = result.find_last_not_of(' ');
    return result.substr(first, last - first + 1U);
}

[[nodiscard]] AssetMetadata raster_metadata(const DecodedJpeg& jpeg) {
    AssetMetadata metadata;
    metadata.make = clean_exif_string(jpeg.exif.make);
    metadata.model = clean_exif_string(jpeg.exif.model);
    metadata.normalized_make = metadata.make;
    metadata.normalized_model = metadata.model;
    // The pixel buffer is physically oriented. Do not make later size/orientation code rotate it
    // a second time; the source EXIF orientation is applied during `render_reference_rgb`.
    metadata.orientation = 1;
    metadata.image_dimensions = oriented_dimensions(jpeg.stored_dimensions, jpeg.exif.orientation);
    metadata.cfa_pattern = "none/rendered-raster";
    metadata.iso_speed = jpeg.exif.iso_speed;
    metadata.exposure_time_seconds = jpeg.exif.exposure_time_seconds;
    metadata.aperture_f_number = jpeg.exif.aperture_f_number;
    metadata.focal_length_mm = jpeg.exif.focal_length_mm;
    metadata.has_gps_coordinates = jpeg.exif.has_gps_coordinates;
    metadata.gps_latitude_degrees = jpeg.exif.gps_latitude_degrees;
    metadata.gps_longitude_degrees = jpeg.exif.gps_longitude_degrees;
    metadata.has_gps_altitude = jpeg.exif.has_gps_altitude;
    metadata.gps_altitude_meters = jpeg.exif.gps_altitude_meters;
    metadata.lens_make = clean_exif_string(jpeg.exif.lens_make);
    metadata.lens_model = clean_exif_string(jpeg.exif.lens_model);
    metadata.focal_length_35mm = jpeg.exif.focal_length_35mm;
    return metadata;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> output_coordinate(
    const std::uint16_t orientation,
    const Dimensions stored,
    const std::uint32_t source_x,
    const std::uint32_t source_y
) noexcept {
    switch (orientation) {
    case 2U:
        return {stored.width - 1U - source_x, source_y};
    case 3U:
        return {stored.width - 1U - source_x, stored.height - 1U - source_y};
    case 4U:
        return {source_x, stored.height - 1U - source_y};
    case 5U:
        return {source_y, source_x};
    case 6U:
        return {stored.height - 1U - source_y, source_x};
    case 7U:
        return {stored.height - 1U - source_y, stored.width - 1U - source_x};
    case 8U:
        return {source_y, stored.width - 1U - source_x};
    case 1U:
    default:
        return {source_x, source_y};
    }
}

[[nodiscard]] PixelBuffer pixel_buffer_from_jpeg(const DecodedJpeg& jpeg) {
    if (jpeg.rgb.empty() || !dimensions_within_limits(jpeg.decoded_dimensions)) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "JPEG decoder returned no RGB pixels");
    }
    const Dimensions output_dimensions = oriented_dimensions(
        jpeg.decoded_dimensions,
        jpeg.exif.orientation
    );
    const std::uint64_t output_samples = output_dimensions.pixel_count() * 3U;
    if (output_samples > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "JPEG output allocation overflows");
    }
    const IccProfile source_profile = jpeg.icc.empty()
        ? make_display_srgb_icc_profile()
        : load_icc_profile(std::as_bytes(std::span(jpeg.icc)));
    const IccProfile destination_profile = make_linear_srgb_icc_profile();
    const IccTransform transform = make_icc_transform(
        source_profile,
        destination_profile,
        IccRenderingIntent::relative_colorimetric,
        true
    );

    PixelBuffer output;
    output.dimensions = output_dimensions;
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes = static_cast<std::size_t>(output_dimensions.width) * 3U
        * sizeof(std::uint16_t);
    output.primaries = RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = RgbTransferFunction::linear;
    output.reference = RgbBufferReference::decoded_raster;
    output.samples.resize(static_cast<std::size_t>(output_samples));

    const std::size_t source_width = jpeg.decoded_dimensions.width;
    std::vector<float> transformed_row(source_width * 3U);
    for (std::uint32_t source_y = 0U; source_y < jpeg.decoded_dimensions.height; ++source_y) {
        for (std::uint32_t source_x = 0U; source_x < jpeg.decoded_dimensions.width; ++source_x) {
            const std::size_t source_index =
                (static_cast<std::size_t>(source_y) * source_width + source_x) * 3U;
            const std::size_t row_index = static_cast<std::size_t>(source_x) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                transformed_row[row_index + channel] = static_cast<float>(
                    static_cast<double>(jpeg.rgb[source_index + channel]) / 255.0
                );
            }
        }
        transform.apply_interleaved_rgb(transformed_row);
        for (std::uint32_t source_x = 0U; source_x < jpeg.decoded_dimensions.width; ++source_x) {
            const auto [output_x, output_y] = output_coordinate(
                jpeg.exif.orientation,
                jpeg.decoded_dimensions,
                source_x,
                source_y
            );
            const std::size_t input_index = static_cast<std::size_t>(source_x) * 3U;
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * output_dimensions.width + output_x) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                output.samples[output_index + channel] = static_cast<std::uint16_t>(std::clamp(
                    std::llround(std::clamp(
                        static_cast<double>(transformed_row[input_index + channel]),
                        0.0,
                        1.0
                    ) * 65'535.0),
                    0LL,
                    65'535LL
                ));
            }
        }
    }
    return output;
}

[[nodiscard]] bool has_extension(
    const std::filesystem::path& path,
    const std::initializer_list<std::string_view> extensions
) {
    const auto extension = path.extension().u8string();
    if (extension.size() <= 1U) {
        return false;
    }
    std::string lower;
    lower.reserve(extension.size() - 1U);
    for (std::size_t index = 1U; index < extension.size(); ++index) {
        const char8_t byte = extension[index];
        lower.push_back(static_cast<char>(
            byte >= u8'A' && byte <= u8'Z' ? byte - u8'A' + u8'a' : byte
        ));
    }
    return std::ranges::find(extensions, std::string_view(lower)) != extensions.end();
}

class RasterSession final : public DecodeSession {
public:
    explicit RasterSession(std::filesystem::path path)
        : path_(std::move(path)) {
        if (!has_extension(path_, {"jpg", "jpeg"})) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "raster provider currently supports JPEG input only"
            );
        }
        const DecodedJpeg header = decode_jpeg_file(path_, false);
        metadata_ = raster_metadata(header);
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] PreviewPayload decode_preview(std::size_t) override {
        throw DecodeError(DecodeErrorCode::no_preview, 0, "JPEG has no separate embedded preview");
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "JPEG is a rendered raster and has no sensor RAW frame"
        );
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return pixel_buffer_from_jpeg(decode_jpeg_file(path_, true));
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        if (max_edge == 0U || max_edge > maximum_preview_edge) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "JPEG preview max edge must be in 1..=4096"
            );
        }
        return pixel_buffer_from_jpeg(decode_jpeg_file(path_, true, max_edge));
    }

private:
    std::filesystem::path path_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
};

class RasterProvider final : public DecoderProvider {
public:
    RasterProvider() {
        info_.id = "shadow-raster";
        info_.version = "jpeg-libjpeg-turbo-" SHADOW_STRINGIFY(LIBJPEG_TURBO_VERSION)
            ";contract=" + std::to_string(raster_decoder_contract_version)
            + ";heif=" + heif_decoder_version()
            + ";display=" + std::to_string(display_srgb8_output_transform_version);
        info_.jpeg = true;
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const override {
        if (has_extension(path, {"heic", "heif"})) {
            return open_heif_decode_session(path);
        }
        return std::make_unique<RasterSession>(path);
    }

private:
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<DecoderProvider> make_raster_decoder_provider() {
    return std::make_unique<RasterProvider>();
}

std::vector<std::string> raster_supported_file_extensions() {
    std::vector<std::string> extensions{"jpg", "jpeg"};
    if (heif_decoder_available()) {
        extensions.emplace_back("heic");
        extensions.emplace_back("heif");
    }
    return extensions;
}

} // namespace shadow::image

#undef SHADOW_STRINGIFY
#undef SHADOW_STRINGIFY_IMPL
