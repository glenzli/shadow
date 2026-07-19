#include <shadow/image/decoder.hpp>

#include <libraw/libraw.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace shadow::image {

namespace {

using ProcessedImage = std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)>;

[[nodiscard]] DecodeErrorCode map_libraw_error(const int result) noexcept {
    switch (result) {
    case LIBRAW_FILE_UNSUPPORTED:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
        return DecodeErrorCode::unsupported;
    case LIBRAW_IO_ERROR:
    case LIBRAW_INPUT_CLOSED:
        return DecodeErrorCode::io;
    case LIBRAW_DATA_ERROR:
        return DecodeErrorCode::corrupt_data;
    case LIBRAW_NO_THUMBNAIL:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL:
        return DecodeErrorCode::no_preview;
    case LIBRAW_UNSUPPORTED_THUMBNAIL:
    case LIBRAW_NOT_IMPLEMENTED:
        return DecodeErrorCode::unsupported_layout;
    case LIBRAW_TOO_BIG:
    case LIBRAW_UNSUFFICIENT_MEMORY:
    case LIBRAW_MEMPOOL_OVERFLOW:
        return DecodeErrorCode::resource_limit;
    case LIBRAW_CANCELLED_BY_CALLBACK:
        return DecodeErrorCode::cancelled;
    case LIBRAW_OUT_OF_ORDER_CALL:
    case LIBRAW_BAD_CROP:
        return DecodeErrorCode::invalid_request;
    default:
        return DecodeErrorCode::internal;
    }
}

[[noreturn]] void throw_libraw_error(const int result, const std::string_view operation) {
    std::ostringstream message;
    message << operation << " failed: " << libraw_strerror(result) << " (" << result << ')';
    throw DecodeError(map_libraw_error(result), result, message.str());
}

void require_libraw_success(const int result, const std::string_view operation) {
    if (result != LIBRAW_SUCCESS) {
        throw_libraw_error(result, operation);
    }
}

[[nodiscard]] int open_path(LibRaw& decoder, const std::filesystem::path& path) {
#if defined(_WIN32)
    return decoder.open_file(path.c_str());
#else
    const std::string native_path = path.native();
    return decoder.open_file(native_path.c_str());
#endif
}

[[nodiscard]] std::string dng_version_string(const unsigned version) {
    if (version == 0U) {
        return {};
    }

    std::ostringstream output;
    output << ((version >> 24U) & 0xffU) << '.' << ((version >> 16U) & 0xffU) << '.'
           << ((version >> 8U) & 0xffU) << '.' << (version & 0xffU);
    return output.str();
}

[[nodiscard]] std::string cfa_pattern(LibRaw& decoder) {
    if (decoder.imgdata.idata.filters == 0U) {
        return "none/linear";
    }

    std::string result;
    result.reserve(4);
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const int color_index = decoder.COLOR(row, column);
            const bool valid_index = color_index >= 0 && color_index < 4;
            result.push_back(valid_index ? decoder.imgdata.idata.cdesc[color_index] : '?');
        }
    }
    return result;
}

[[nodiscard]] PreviewFormat preview_format(const LibRaw_internal_thumbnail_formats format) noexcept {
    switch (format) {
    case LIBRAW_INTERNAL_THUMBNAIL_JPEG:
        return PreviewFormat::jpeg;
    case LIBRAW_INTERNAL_THUMBNAIL_JPEGXL:
        return PreviewFormat::jpeg_xl;
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_YCBCR:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM16:
    case LIBRAW_INTERNAL_THUMBNAIL_DNG_YCBCR:
        return PreviewFormat::bitmap;
    default:
        return PreviewFormat::unknown;
    }
}

[[nodiscard]] PreviewFormat preview_format(const LibRaw_image_formats format) noexcept {
    switch (format) {
    case LIBRAW_IMAGE_JPEG:
        return PreviewFormat::jpeg;
    case LIBRAW_IMAGE_BITMAP:
        return PreviewFormat::bitmap;
    case LIBRAW_IMAGE_JPEGXL:
        return PreviewFormat::jpeg_xl;
    case LIBRAW_IMAGE_H265:
        return PreviewFormat::h265;
    default:
        return PreviewFormat::unknown;
    }
}

[[nodiscard]] Margins image_margins(const libraw_image_sizes_t& sizes) noexcept {
    const auto used_width = static_cast<std::uint32_t>(sizes.left_margin) + sizes.width;
    const auto used_height = static_cast<std::uint32_t>(sizes.top_margin) + sizes.height;
    const auto right = sizes.raw_width > used_width ? sizes.raw_width - used_width : 0U;
    const auto bottom = sizes.raw_height > used_height ? sizes.raw_height - used_height : 0U;
    return Margins{sizes.left_margin, sizes.top_margin, right, bottom};
}

[[nodiscard]] PendingCorrections pending_corrections(const libraw_data_t& data) noexcept {
    const auto& opcodes = data.color.dng_levels.rawopcodes;
    return PendingCorrections{{opcodes[0].len, opcodes[1].len, opcodes[2].len}};
}

[[nodiscard]] AssetMetadata read_metadata(LibRaw& decoder) {
    const auto& identity = decoder.imgdata.idata;
    const auto& sizes = decoder.imgdata.sizes;
    const auto& color = decoder.imgdata.color;
    const auto& dng = color.dng_levels;

    AssetMetadata metadata;
    metadata.make = identity.make;
    metadata.model = identity.model;
    metadata.normalized_make = identity.normalized_make;
    metadata.normalized_model = identity.normalized_model;
    metadata.dng_version = dng_version_string(identity.dng_version);
    metadata.raw_count = identity.raw_count;
    metadata.raw_dimensions = Dimensions{sizes.raw_width, sizes.raw_height};
    metadata.image_dimensions = Dimensions{sizes.width, sizes.height};
    metadata.margins = image_margins(sizes);
    metadata.orientation = sizes.flip;
    metadata.cfa_pattern = cfa_pattern(decoder);
    metadata.sensor_colors = static_cast<std::uint32_t>(identity.colors);
    metadata.sensor_bits = color.raw_bps;
    metadata.black_level = color.black;
    metadata.white_level = color.maximum;
    for (std::size_t index = 0; index < metadata.as_shot_neutral.size(); ++index) {
        metadata.as_shot_neutral[index] = dng.asshotneutral[index];
    }
    metadata.baseline_exposure = dng.baseline_exposure;
    return metadata;
}

[[nodiscard]] std::vector<PreviewDescriptor> read_previews(const libraw_data_t& data) {
    std::vector<PreviewDescriptor> previews;
    const int count = std::max(0, data.thumbs_list.thumbcount);
    previews.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        const auto& candidate = data.thumbs_list.thumblist[index];
        const bool placeholder =
            candidate.tlength == 0U && candidate.twidth == 0U && candidate.theight == 0U;
        if (placeholder) {
            continue;
        }

        PreviewDescriptor descriptor;
        descriptor.id = static_cast<std::size_t>(index);
        descriptor.format = preview_format(candidate.tformat);
        descriptor.dimensions = Dimensions{candidate.twidth, candidate.theight};
        descriptor.bits_per_channel = static_cast<std::uint16_t>(candidate.tmisc & 31U);
        descriptor.channels = static_cast<std::uint16_t>(candidate.tmisc >> 5U);
        descriptor.encoded_bytes = candidate.tlength;
        descriptor.decodable = descriptor.format != PreviewFormat::unknown;
        previews.push_back(std::move(descriptor));
    }
    return previews;
}

class LibRawSession final : public DecodeSession {
public:
    explicit LibRawSession(std::filesystem::path path) : path_(std::move(path)) {
        decoder_.imgdata.rawparams.max_raw_memory_mb = 2'048U;
        require_libraw_success(open_path(decoder_, path_), "open_file");

        metadata_ = read_metadata(decoder_);
        previews_ = read_previews(decoder_.imgdata);
        capabilities_.metadata = true;
        capabilities_.embedded_previews = !previews_.empty();
        capabilities_.mosaic =
            decoder_.imgdata.idata.filters != 0U || decoder_.imgdata.idata.colors == 1;
        capabilities_.reference_rgb = true;
        capabilities_.pending_corrections = pending_corrections(decoder_.imgdata);
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return previews_;
    }

    [[nodiscard]] PreviewPayload decode_preview(const std::size_t id) override {
        const auto descriptor = std::find_if(
            previews_.begin(),
            previews_.end(),
            [id](const PreviewDescriptor& candidate) { return candidate.id == id; }
        );
        if (descriptor == previews_.end() || !descriptor->decodable || id > INT_MAX) {
            throw DecodeError(
                DecodeErrorCode::no_preview,
                LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL,
                "requested preview is not available"
            );
        }

        require_libraw_success(decoder_.unpack_thumb_ex(static_cast<int>(id)), "unpack_thumb_ex");
        int result = LIBRAW_SUCCESS;
        ProcessedImage image(
            decoder_.dcraw_make_mem_thumb(&result),
            &LibRaw::dcraw_clear_mem
        );
        if (!image) {
            throw_libraw_error(result, "dcraw_make_mem_thumb");
        }

        PreviewPayload payload;
        payload.descriptor = *descriptor;
        payload.descriptor.format = preview_format(image->type);
        if (image->width != 0U && image->height != 0U) {
            payload.descriptor.dimensions = Dimensions{image->width, image->height};
        }
        if (image->bits != 0U) {
            payload.descriptor.bits_per_channel = image->bits;
        }
        if (image->colors != 0U) {
            payload.descriptor.channels = image->colors;
        }
        payload.descriptor.encoded_bytes = image->data_size;
        payload.descriptor.decodable = payload.descriptor.format != PreviewFormat::unknown;
        payload.byte_order =
            payload.descriptor.format == PreviewFormat::bitmap && image->bits == 16U
                ? ByteOrder::native
                : ByteOrder::not_applicable;
        payload.bytes.assign(image->data, image->data + image->data_size);
        return payload;
    }

    [[nodiscard]] MosaicBuffer decode_mosaic() override {
        ensure_unpacked();
        const auto& sizes = decoder_.imgdata.sizes;
        const auto* raw_image = decoder_.imgdata.rawdata.raw_image;
        if (raw_image == nullptr) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "decoder did not return a single-plane integer mosaic"
            );
        }

        const std::size_t width = sizes.raw_width;
        const std::size_t height = sizes.raw_height;
        if (height != 0U && width > std::numeric_limits<std::size_t>::max() / height) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "mosaic dimensions overflow the address space"
            );
        }

        const std::size_t source_stride =
            sizes.raw_pitch == 0U ? width : sizes.raw_pitch / sizeof(std::uint16_t);
        if (source_stride < width || sizes.raw_pitch % sizeof(std::uint16_t) != 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_DATA_ERROR,
                "mosaic row stride is invalid"
            );
        }

        MosaicBuffer buffer;
        buffer.descriptor.raw_dimensions = metadata_.raw_dimensions;
        buffer.descriptor.image_dimensions = metadata_.image_dimensions;
        buffer.descriptor.margins = metadata_.margins;
        buffer.descriptor.cfa_pattern = metadata_.cfa_pattern;
        buffer.descriptor.bits_per_sample = metadata_.sensor_bits;
        buffer.descriptor.black_level = metadata_.black_level;
        buffer.descriptor.white_level = metadata_.white_level;
        buffer.descriptor.row_stride_bytes = width * sizeof(std::uint16_t);
        buffer.descriptor.pending_corrections = capabilities_.pending_corrections;
        buffer.samples.resize(width * height);

        for (std::size_t row = 0; row < height; ++row) {
            const auto* source = raw_image + (row * source_stride);
            auto* destination = buffer.samples.data() + (row * width);
            std::copy_n(source, width, destination);
        }
        return buffer;
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        LibRaw renderer;
        renderer.imgdata.rawparams.max_raw_memory_mb = 2'048U;
        require_libraw_success(open_path(renderer, path_), "reference open_file");
        require_libraw_success(renderer.unpack(), "reference unpack");

        auto& parameters = renderer.imgdata.params;
        parameters.output_bps = 16;
        parameters.use_camera_wb = 1;
        parameters.no_auto_bright = 1;
        parameters.output_color = 1;
        parameters.user_qual = 3;
        require_libraw_success(renderer.dcraw_process(), "dcraw_process");

        int result = LIBRAW_SUCCESS;
        ProcessedImage image(
            renderer.dcraw_make_mem_image(&result),
            &LibRaw::dcraw_clear_mem
        );
        if (!image) {
            throw_libraw_error(result, "dcraw_make_mem_image");
        }
        if (
            image->type != LIBRAW_IMAGE_BITMAP || image->bits != 16U
            || (image->colors != 1U && image->colors != 3U)
        ) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "reference renderer returned an unsupported pixel layout"
            );
        }

        const std::size_t width = image->width;
        const std::size_t height = image->height;
        const std::size_t channels = image->colors;
        const bool pixel_count_overflows =
            height != 0U && width > std::numeric_limits<std::size_t>::max() / height;
        if (pixel_count_overflows) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "reference image dimensions overflow the address space"
            );
        }

        const std::size_t pixel_count = width * height;
        if (
            channels != 0U
            && pixel_count > std::numeric_limits<std::size_t>::max() / channels
        ) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "reference image channel count overflows the address space"
            );
        }

        const std::size_t sample_count = pixel_count * channels;
        if (sample_count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "reference image byte count overflows the address space"
            );
        }
        const std::size_t byte_count = sample_count * sizeof(std::uint16_t);
        if (image->data_size < byte_count) {
            throw DecodeError(
                DecodeErrorCode::corrupt_data,
                LIBRAW_DATA_ERROR,
                "reference image buffer is shorter than its descriptor"
            );
        }

        PixelBuffer buffer;
        buffer.dimensions = Dimensions{image->width, image->height};
        buffer.bits_per_channel = image->bits;
        buffer.channels = image->colors;
        buffer.row_stride_bytes = width * channels * sizeof(std::uint16_t);
        buffer.color_space = ColorSpace::srgb;
        buffer.samples.resize(sample_count);
        std::memcpy(buffer.samples.data(), image->data, byte_count);
        return buffer;
    }

private:
    void ensure_unpacked() {
        if (unpacked_) {
            return;
        }
        require_libraw_success(decoder_.unpack(), "unpack");
        unpacked_ = true;
    }

    std::filesystem::path path_;
    LibRaw decoder_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
    std::vector<PreviewDescriptor> previews_;
    bool unpacked_ = false;
};

class LibRawProvider final : public DecoderProvider {
public:
    LibRawProvider() {
        const unsigned capabilities = LibRaw::capabilities();
        info_.id = "libraw";
        info_.version = LibRaw::version();
        info_.dng_sdk = (capabilities & LIBRAW_CAPS_DNGSDK) != 0U;
        info_.rawspeed =
            (capabilities & (LIBRAW_CAPS_RAWSPEED | LIBRAW_CAPS_RAWSPEED3)) != 0U;
        info_.jpeg = (capabilities & LIBRAW_CAPS_JPEG) != 0U;
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const override {
        return std::make_unique<LibRawSession>(path);
    }

private:
    ProviderInfo info_;
};

} // namespace

std::uint64_t Dimensions::pixel_count() const noexcept {
    return static_cast<std::uint64_t>(width) * height;
}

bool PendingCorrections::has_pending() const noexcept {
    return std::ranges::any_of(dng_opcode_list_bytes, [](const std::uint32_t size) {
        return size != 0U;
    });
}

DecodeError::DecodeError(
    const DecodeErrorCode code,
    const int provider_code,
    std::string message
)
    : std::runtime_error(std::move(message)), code_(code), provider_code_(provider_code) {}

DecodeErrorCode DecodeError::code() const noexcept {
    return code_;
}

int DecodeError::provider_code() const noexcept {
    return provider_code_;
}

std::unique_ptr<DecoderProvider> make_libraw_decoder_provider() {
    return std::make_unique<LibRawProvider>();
}

std::optional<std::size_t> select_best_preview(
    const std::span<const PreviewDescriptor> previews
) noexcept {
    const PreviewDescriptor* best = nullptr;
    for (const auto& candidate : previews) {
        if (!candidate.decodable) {
            continue;
        }
        if (
            best == nullptr || candidate.dimensions.pixel_count() > best->dimensions.pixel_count()
            || (
                candidate.dimensions.pixel_count() == best->dimensions.pixel_count()
                && candidate.encoded_bytes > best->encoded_bytes
            )
        ) {
            best = &candidate;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<std::size_t>{best->id};
}

std::string_view to_string(const PreviewFormat format) noexcept {
    switch (format) {
    case PreviewFormat::jpeg:
        return "jpeg";
    case PreviewFormat::bitmap:
        return "bitmap";
    case PreviewFormat::jpeg_xl:
        return "jpeg-xl";
    case PreviewFormat::h265:
        return "h265";
    case PreviewFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace shadow::image
