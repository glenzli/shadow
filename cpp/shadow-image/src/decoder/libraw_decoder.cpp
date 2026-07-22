#include <shadow/image/decoder.hpp>

#include <libraw/libraw.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

namespace shadow::image {

namespace {

using ProcessedImage = std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)>;
inline constexpr std::uint32_t libraw_capability_contract_version = 3U;
inline constexpr int libraw_reference_output_color = 1;
inline constexpr double libraw_reference_gamma_inverse_power = 1.0;
inline constexpr double libraw_reference_gamma_linear_toe_slope = 1.0;

// This is intentionally distinct from the verbose receipt signature below. Provider versions
// become part of catalog ContentIdentity, whose text fields are capped at 128 bytes. Preserve
// every output-affecting LibRaw setting in a compact, reversible form rather than letting an
// explanatory sentence make a valid cache identity impossible to persist.
[[nodiscard]] std::string compact_libraw_development_settings_identity(
    const LibRawDevelopmentSettings& settings
) {
    std::ostringstream identity;
    identity << "s" << settings.schema_version
             << "-w" << (settings.use_camera_white_balance ? 1 : 0)
             << "-m" << (settings.use_camera_matrix ? 1 : 0)
             << "-a" << (settings.use_auto_brightness ? 1 : 0)
             << "-e" << (settings.use_exposure_correction ? 1 : 0)
             << "-b" << std::hex << std::bit_cast<std::uint32_t>(settings.brightness)
             << "-x" << std::bit_cast<std::uint32_t>(settings.maximum_adjustment_threshold)
             << std::dec << "-p" << settings.output_bits_per_channel
             << "-q" << settings.demosaic_quality;
    return identity.str();
}

void configure_reference_render_parameters(
    LibRaw& renderer,
    const LibRawDevelopmentSettings& settings,
    const bool half_size
) {
    auto& parameters = renderer.imgdata.params;
    // These requests are deliberately installed before open/unpack. LibRaw documents that
    // camera WB/matrix and half-size can affect earlier decode stages for some formats, so doing
    // it only before dcraw_process would make the receipt claim more than the decoder guaranteed.
    parameters.gamm[0] = libraw_reference_gamma_inverse_power;
    parameters.gamm[1] = libraw_reference_gamma_linear_toe_slope;
    parameters.output_bps = static_cast<int>(settings.output_bits_per_channel);
    parameters.use_camera_wb = settings.use_camera_white_balance ? 1 : 0;
    parameters.use_camera_matrix = settings.use_camera_matrix ? 1 : 0;
    parameters.bright = settings.brightness;
    parameters.exp_correc = settings.use_exposure_correction ? 1 : 0;
    parameters.no_auto_bright = settings.use_auto_brightness ? 0 : 1;
    parameters.adjust_maximum_thr = settings.maximum_adjustment_threshold;
    parameters.output_color = libraw_reference_output_color;
    parameters.user_qual = static_cast<int>(settings.demosaic_quality);
    parameters.half_size = half_size ? 1 : 0;
}

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

void validate_development_settings(const LibRawDevelopmentSettings& settings) {
    if (settings.schema_version != libraw_development_settings_schema_version) {
        throw std::invalid_argument("unsupported LibRaw development settings schema version");
    }
    if (!std::isfinite(settings.brightness) || settings.brightness <= 0.0F
        || settings.brightness > 8.0F) {
        throw std::invalid_argument("LibRaw brightness must be finite and in (0, 8]");
    }
    if (!std::isfinite(settings.maximum_adjustment_threshold)
        || settings.maximum_adjustment_threshold < 0.0F
        || settings.maximum_adjustment_threshold > 1.0F) {
        throw std::invalid_argument(
            "LibRaw maximum adjustment threshold must be finite and in [0, 1]"
        );
    }
    if (settings.output_bits_per_channel != 16U) {
        throw std::invalid_argument("Shadow's processed-linear LibRaw contract requires 16-bit output");
    }
    if (settings.demosaic_quality < 0 || settings.demosaic_quality > 13) {
        throw std::invalid_argument("LibRaw demosaic quality must be in [0, 13]");
    }
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
    const auto& capture = decoder.imgdata.other;
    const auto& lens = decoder.imgdata.lens;

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
    metadata.iso_speed = capture.iso_speed;
    metadata.exposure_time_seconds = capture.shutter;
    metadata.aperture_f_number = capture.aperture;
    metadata.focal_length_mm = capture.focal_len;
    metadata.captured_at_unix_seconds = static_cast<std::int64_t>(capture.timestamp);
    metadata.lens_make = lens.LensMake;
    metadata.lens_model = lens.Lens;
    metadata.focal_length_35mm = lens.FocalLengthIn35mmFormat;
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
    explicit LibRawSession(
        std::filesystem::path path,
        LibRawDevelopmentSettings settings,
        ProviderInfo provider_info
    )
        : path_(std::move(path)), settings_(settings), provider_info_(std::move(provider_info)) {
        decoder_.imgdata.rawparams.max_raw_memory_mb = 2'048U;
        require_libraw_success(open_path(decoder_, path_), "open_file");

        metadata_ = read_metadata(decoder_);
        previews_ = read_previews(decoder_.imgdata);
        libraw_decoder_info_t decoder_info{};
        const bool decoder_can_unpack =
            decoder_.get_decoder_info(&decoder_info) == LIBRAW_SUCCESS
            && (decoder_info.decoder_flags
                & (LIBRAW_DECODER_UNSUPPORTED_FORMAT | LIBRAW_DECODER_NOTSET)) == 0U;
        capabilities_.metadata = true;
        capabilities_.embedded_previews = !previews_.empty();
        capabilities_.mosaic = decoder_can_unpack
            && (decoder_.imgdata.idata.filters != 0U
                || decoder_.imgdata.idata.colors == 1);
        capabilities_.reference_rgb = decoder_can_unpack;
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
        return render_reference_rgb_impl(false);
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        if (max_edge == 0U) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                LIBRAW_BAD_CROP,
                "preview reference edge must be non-zero"
            );
        }
        const std::uint64_t native_edge = std::max(
            static_cast<std::uint64_t>(metadata_.image_dimensions.width),
            static_cast<std::uint64_t>(metadata_.image_dimensions.height)
        );
        // LibRaw's half-size mode is a genuine reduced demosaic path, not a post-process
        // resize. It avoids spending full-resolution CPU and memory bandwidth on a source whose
        // next step is a 1200–2048 px interactive proxy. Keep full quality for smaller files and
        // for any request where a half-size raster would not materially reduce work.
        const bool use_half_size = native_edge > static_cast<std::uint64_t>(max_edge) * 2U;
        return render_reference_rgb_impl(use_half_size);
    }

private:
    [[nodiscard]] PixelBuffer render_reference_rgb_impl(const bool half_size) const {
        // LibRaw embeds sizeable fixed storage in the decoder object. QtConcurrent worker
        // threads use a substantially smaller stack than the process main thread on macOS,
        // so keeping a temporary LibRaw here can overflow the worker before open_file runs.
        // The renderer is independent state and belongs on the heap regardless of caller.
        auto renderer = std::make_unique<LibRaw>();
        renderer->imgdata.rawparams.max_raw_memory_mb = 2'048U;
        configure_reference_render_parameters(*renderer, settings_, half_size);
        require_libraw_success(open_path(*renderer, path_), "reference open_file");
        require_libraw_success(renderer->unpack(), "reference unpack");

        // LibRaw may update `sizes.width` and `sizes.height` while producing its output bitmap.
        // Snapshot the source declaration before `dcraw_process()` so a half-size receipt can
        // distinguish its original image geometry from the rendered raster.
        const Dimensions declared_image_dimensions{
            renderer->imgdata.sizes.width,
            renderer->imgdata.sizes.height,
        };
        const std::int32_t declared_orientation = renderer->imgdata.sizes.flip;
        const PendingCorrections declared_dng_opcode_lists = pending_corrections(renderer->imgdata);
        // Re-assert the same request after unpack as a defensive guard against a LibRaw build
        // that initializes a postprocess default while loading metadata. The pre-open call above
        // remains the important one for formats where these switches influence loading itself.
        configure_reference_render_parameters(*renderer, settings_, half_size);
        auto& parameters = renderer->imgdata.params;
        require_libraw_success(renderer->dcraw_process(), "dcraw_process");

        int result = LIBRAW_SUCCESS;
        ProcessedImage image(
            renderer->dcraw_make_mem_image(&result),
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
        buffer.primaries = RgbPrimaries::srgb_rec709_d65;
        buffer.transfer_function = RgbTransferFunction::linear;
        buffer.reference = RgbBufferReference::processed_raw;
        buffer.samples.resize(sample_count);
        std::memcpy(buffer.samples.data(), image->data, byte_count);
        buffer.raw_development_receipt = RawDevelopmentReceipt{
            .schema_version = raw_development_receipt_schema_version,
            .provider_id = provider_info_.id,
            .provider_version = provider_info_.version,
            .library_version = std::string(LibRaw::version()),
            .development_settings_signature = libraw_development_settings_signature(settings_),
            .processed_linear_reference_contract_version =
                processed_linear_reference_rgb_contract_version,
            .declared_image_dimensions = declared_image_dimensions,
            .rendered_dimensions = buffer.dimensions,
            .orientation = declared_orientation,
            .half_size = half_size,
            .use_camera_white_balance = parameters.use_camera_wb != 0,
            .use_camera_matrix = parameters.use_camera_matrix != 0,
            .use_auto_brightness = parameters.no_auto_bright == 0,
            .use_exposure_correction = parameters.exp_correc != 0,
            .brightness = parameters.bright,
            .maximum_adjustment_threshold = parameters.adjust_maximum_thr,
            .output_bits_per_channel = static_cast<std::uint16_t>(parameters.output_bps),
            .demosaic_quality = parameters.user_qual,
            .output_color = parameters.output_color,
            .gamma_inverse_power = parameters.gamm[0],
            .gamma_linear_toe_slope = parameters.gamm[1],
            .declared_dng_opcode_lists = declared_dng_opcode_lists,
            .process_warnings = renderer->imgdata.process_warnings,
        };
        return buffer;
    }

    void ensure_unpacked() {
        if (unpacked_) {
            return;
        }
        require_libraw_success(decoder_.unpack(), "unpack");
        unpacked_ = true;
    }

    std::filesystem::path path_;
    LibRawDevelopmentSettings settings_;
    ProviderInfo provider_info_;
    LibRaw decoder_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
    std::vector<PreviewDescriptor> previews_;
    bool unpacked_ = false;
};

class LibRawProvider final : public DecoderProvider {
public:
    explicit LibRawProvider(LibRawDevelopmentSettings settings) : settings_(settings) {
        validate_development_settings(settings_);
        const unsigned capabilities = LibRaw::capabilities();
        info_.id = "libraw";
        // Provider version participates in generated-proxy/cache identity. Include Shadow's
        // reference/output contracts so a transfer or gamut-mapping change cannot reuse bytes
        // generated under the same linked LibRaw release.
        info_.version = "libraw=" + std::string(LibRaw::version())
            + ";cap=" + std::to_string(libraw_capability_contract_version)
            + ";linear=" + std::to_string(processed_linear_reference_rgb_contract_version)
            + ";receipt=" + std::to_string(raw_development_receipt_schema_version)
            + ";display=" + std::to_string(display_srgb8_output_transform_version)
            + ";settings=" + compact_libraw_development_settings_identity(settings_);
        if (info_.version.size() > 128U) {
            throw std::invalid_argument("LibRaw provider cache identity exceeds 128 bytes");
        }
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
        return std::make_unique<LibRawSession>(path, settings_, info_);
    }

private:
    LibRawDevelopmentSettings settings_;
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

LibRawDevelopmentSettings default_libraw_development_settings() noexcept {
    return LibRawDevelopmentSettings{
        .schema_version = libraw_development_settings_schema_version,
        .use_camera_white_balance = true,
        .use_camera_matrix = true,
        .use_auto_brightness = false,
        .use_exposure_correction = false,
        .brightness = 1.0F,
        .maximum_adjustment_threshold = processed_linear_reference_maximum_adjustment_threshold,
        .output_bits_per_channel = 16U,
        .demosaic_quality = 3,
    };
}

std::string libraw_development_settings_signature(const LibRawDevelopmentSettings& settings) {
    std::ostringstream signature;
    signature << "shadow-libraw-develop-v" << settings.schema_version
              << ";wb=" << (settings.use_camera_white_balance ? "camera" : "none")
              << ";matrix=" << (settings.use_camera_matrix ? "camera" : "none")
              << ";auto-bright=" << (settings.use_auto_brightness ? "on" : "off")
              << ";exposure=" << (settings.use_exposure_correction ? "on" : "off")
              << ";bright=" << settings.brightness
              << ";max-adjust=" << settings.maximum_adjustment_threshold
              << ";bps=" << settings.output_bits_per_channel
              << ";qual=" << settings.demosaic_quality;
    return signature.str();
}

std::unique_ptr<DecoderProvider> make_libraw_decoder_provider(
    const LibRawDevelopmentSettings settings
) {
    return std::make_unique<LibRawProvider>(settings);
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
