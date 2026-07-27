#include "heif_decoder.hpp"
#include "raster_exif.hpp"

#include <shadow/image/color_management.hpp>
#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(SHADOW_IMAGE_HAS_LIBHEIF)
#define SHADOW_IMAGE_HAS_LIBHEIF 0
#endif

#if SHADOW_IMAGE_HAS_LIBHEIF
#include <libheif/heif.h>
#endif

namespace shadow::image {

#if SHADOW_IMAGE_HAS_LIBHEIF

namespace {

inline constexpr std::uint32_t heif_decoder_contract_version = 1U;
inline constexpr std::uint64_t maximum_heif_source_pixels = 50'000'000U;
inline constexpr std::uint32_t maximum_heif_source_dimension = 65'535U;
inline constexpr std::uint64_t maximum_embedded_icc_bytes = 16U * 1024U * 1024U;
inline constexpr std::uint64_t maximum_embedded_exif_bytes = 16U * 1024U * 1024U;

// CICP values are intentionally recorded as their standardized numerical values rather than a
// libheif enum spelling. This keeps the acceptance policy clear: current raster semantics accept
// only SDR sRGB (or linear sRGB), not a visually similar but technically different Rec.709,
// Display-P3, BT.2020, PQ, or HLG source.
inline constexpr int cicp_bt709_primaries = 1;
inline constexpr int cicp_bt709_transfer = 1;
inline constexpr int cicp_linear_transfer = 8;
inline constexpr int cicp_srgb_transfer = 13;

struct HeifContextDeleter final {
    void operator()(heif_context* const context) const noexcept {
        if (context != nullptr) {
            heif_context_free(context);
        }
    }
};

struct HeifHandleDeleter final {
    void operator()(heif_image_handle* const handle) const noexcept {
        if (handle != nullptr) {
            heif_image_handle_release(handle);
        }
    }
};

struct HeifImageDeleter final {
    void operator()(heif_image* const image) const noexcept {
        if (image != nullptr) {
            heif_image_release(image);
        }
    }
};

using HeifContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HeifHandlePtr = std::unique_ptr<heif_image_handle, HeifHandleDeleter>;
using HeifImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;

struct OpenedHeif final {
    HeifContextPtr context;
    HeifHandlePtr handle;
};

struct HeifSourceColor final {
    enum class Transfer : std::uint8_t {
        display_srgb,
        display_rec709,
        linear_srgb,
    };

    std::vector<std::uint8_t> icc;
    Transfer transfer = Transfer::display_srgb;
};

struct DecodedHeif final {
    Dimensions dimensions;
    HeifSourceColor source_color;
    std::vector<std::uint8_t> rgb;
};

[[nodiscard]] std::string path_for_message(const std::filesystem::path& path) {
    const auto native = path.u8string();
    return native.empty()
        ? std::string{"<unnamed file>"}
        : std::string(native.begin(), native.end());
}

[[nodiscard]] bool dimensions_within_limits(const Dimensions dimensions) noexcept {
    return dimensions.width > 0U && dimensions.height > 0U
        && dimensions.width <= maximum_heif_source_dimension
        && dimensions.height <= maximum_heif_source_dimension
        && dimensions.pixel_count() <= maximum_heif_source_pixels;
}

[[nodiscard]] std::string heif_error_message(const heif_error error) {
    return error.message == nullptr || error.message[0] == '\0'
        ? std::string{"unknown libheif error"}
        : std::string(error.message);
}

void require_heif_success(
    const heif_error error,
    const DecodeErrorCode code,
    const std::string_view operation
) {
    if (error.code == heif_error_Ok) {
        return;
    }
    throw DecodeError(
        code,
        static_cast<int>(error.code),
        std::string(operation) + ": " + heif_error_message(error)
    );
}

void ensure_heif_runtime() {
    // libheif's init/deinit pair is reference-counted, and init loads the build's default codec
    // plugin directory. Shadow deliberately keeps that runtime alive for the application process:
    // it avoids unloading a codec while another catalog/edit worker is still decoding.
    static std::once_flag once;
    static std::string initialization_error;
    std::call_once(once, [] {
        const heif_error error = heif_init(nullptr);
        if (error.code != heif_error_Ok) {
            initialization_error = heif_error_message(error);
        }
    });
    if (!initialization_error.empty()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "libheif initialization failed: " + initialization_error
        );
    }
}

void require_hevc_decoder() {
    if (heif_have_decoder_for_format(heif_compression_HEVC) == 0) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "libheif is present but this build has no HEVC/HEIC decoder plugin"
        );
    }
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
    const auto native = path.u8string();
    return std::string(native.begin(), native.end());
}

[[nodiscard]] OpenedHeif open_primary_image(const std::filesystem::path& path) {
    ensure_heif_runtime();
    require_hevc_decoder();

    HeifContextPtr context(heif_context_alloc());
    if (!context) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "libheif could not allocate a decode context"
        );
    }
    const std::string native_path = utf8_path(path);
    require_heif_success(
        heif_context_read_from_file(context.get(), native_path.c_str(), nullptr),
        DecodeErrorCode::corrupt_data,
        "HEIF read failed for " + path_for_message(path)
    );
    heif_image_handle* raw_handle = nullptr;
    require_heif_success(
        heif_context_get_primary_image_handle(context.get(), &raw_handle),
        DecodeErrorCode::corrupt_data,
        "HEIF has no decodable primary image"
    );
    return OpenedHeif{std::move(context), HeifHandlePtr(raw_handle)};
}

[[nodiscard]] Dimensions dimensions_from_handle(const heif_image_handle& handle) {
    const int width = heif_image_handle_get_width(&handle);
    const int height = heif_image_handle_get_height(&handle);
    if (width <= 0 || height <= 0) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "HEIF primary image has no dimensions");
    }
    const Dimensions dimensions{
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
    if (!dimensions_within_limits(dimensions)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "HEIF dimensions exceed Shadow's retained raster limits"
        );
    }
    return dimensions;
}

void reject_unsupported_heif_layout(const heif_image_handle& handle) {
    if (heif_image_handle_has_alpha_channel(&handle) != 0) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "HEIF with alpha is not supported until Shadow has an alpha-aware editing contract"
        );
    }
    const int source_bits = heif_image_handle_get_luma_bits_per_pixel(&handle);
    if (source_bits > 8) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "HEIF 10/12-bit or HDR input requires Shadow's future HDR raster contract"
        );
    }
}

[[nodiscard]] HeifSourceColor source_color_from_handle(const heif_image_handle& handle) {
    const std::size_t icc_bytes = heif_image_handle_get_raw_color_profile_size(&handle);
    if (icc_bytes > maximum_embedded_icc_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "HEIF embedded ICC profile exceeds Shadow's supported size"
        );
    }
    if (icc_bytes != 0U) {
        HeifSourceColor source;
        source.icc.resize(icc_bytes);
        require_heif_success(
            heif_image_handle_get_raw_color_profile(&handle, source.icc.data()),
            DecodeErrorCode::corrupt_data,
            "HEIF embedded ICC profile could not be read"
        );
        return source;
    }

    heif_color_profile_nclx* nclx = nullptr;
    const heif_error nclx_error = heif_image_handle_get_nclx_color_profile(&handle, &nclx);
    if (nclx_error.code != heif_error_Ok) {
        // Only a truly absent profile gets the JPEG-equivalent sRGB fallback. A declared but
        // malformed nclx profile is a corrupt source, not permission to silently change colour.
        if (heif_image_handle_get_color_profile_type(&handle)
            == heif_color_profile_type_not_present) {
            return HeifSourceColor{};
        }
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            static_cast<int>(nclx_error.code),
            "HEIF nclx colour profile could not be read: " + heif_error_message(nclx_error)
        );
    }
    if (nclx == nullptr) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "HEIF returned a null nclx profile");
    }
    const int primaries = static_cast<int>(nclx->color_primaries);
    const int transfer = static_cast<int>(nclx->transfer_characteristics);
    heif_nclx_color_profile_free(nclx);
    if (primaries != cicp_bt709_primaries) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "HEIF non-sRGB primaries require Shadow's future wide-gamut raster contract"
        );
    }
    if (transfer == cicp_srgb_transfer) {
        return HeifSourceColor{};
    }
    if (transfer == cicp_bt709_transfer) {
        return HeifSourceColor{.icc = {}, .transfer = HeifSourceColor::Transfer::display_rec709};
    }
    if (transfer == cicp_linear_transfer) {
        return HeifSourceColor{.icc = {}, .transfer = HeifSourceColor::Transfer::linear_srgb};
    }
    throw DecodeError(
        DecodeErrorCode::unsupported_layout,
        0,
        "HEIF transfer function is not supported SDR sRGB/Rec.709; PQ and HLG need a dedicated input path"
    );
}

void parse_heif_exif(const heif_image_handle& handle, RasterExif& exif) {
    heif_item_id id = 0;
    const int count = heif_image_handle_get_list_of_metadata_block_IDs(
        &handle,
        "Exif",
        &id,
        1
    );
    if (count <= 0) {
        return;
    }
    const std::size_t bytes = heif_image_handle_get_metadata_size(&handle, id);
    if (bytes <= 4U || bytes > maximum_embedded_exif_bytes) {
        // EXIF is optional presentation metadata. Match the JPEG parser's policy: an overlarge
        // or truncated block must not stop an otherwise valid rendered image from opening.
        return;
    }
    std::vector<std::uint8_t> payload(bytes);
    require_heif_success(
        heif_image_handle_get_metadata(&handle, id, payload.data()),
        DecodeErrorCode::corrupt_data,
        "HEIF EXIF metadata could not be read"
    );
    const std::uint32_t tiff_offset = (static_cast<std::uint32_t>(payload[0]) << 24U)
        | (static_cast<std::uint32_t>(payload[1]) << 16U)
        | (static_cast<std::uint32_t>(payload[2]) << 8U)
        | static_cast<std::uint32_t>(payload[3]);
    const std::size_t remaining = payload.size() - 4U;
    if (tiff_offset > remaining) {
        return;
    }
    parse_tiff_exif(
        std::span<const std::uint8_t>(payload).subspan(4U + tiff_offset),
        exif
    );
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

[[nodiscard]] AssetMetadata metadata_from_handle(const heif_image_handle& handle) {
    reject_unsupported_heif_layout(handle);
    static_cast<void>(source_color_from_handle(handle));
    RasterExif exif;
    parse_heif_exif(handle, exif);
    AssetMetadata metadata;
    metadata.make = clean_exif_string(exif.make);
    metadata.model = clean_exif_string(exif.model);
    metadata.normalized_make = metadata.make;
    metadata.normalized_model = metadata.model;
    metadata.image_dimensions = dimensions_from_handle(handle);
    // libheif applies HEIF crop/rotation/mirror transformations at decode time. Keeping the
    // catalog orientation neutral prevents a second rotation later in Shadow's proxy/editor.
    metadata.orientation = 1;
    metadata.cfa_pattern = "none/rendered-heif";
    metadata.iso_speed = exif.iso_speed;
    metadata.exposure_time_seconds = exif.exposure_time_seconds;
    metadata.aperture_f_number = exif.aperture_f_number;
    metadata.focal_length_mm = exif.focal_length_mm;
    metadata.lens_make = clean_exif_string(exif.lens_make);
    metadata.lens_model = clean_exif_string(exif.lens_model);
    metadata.focal_length_35mm = exif.focal_length_35mm;
    return metadata;
}

[[nodiscard]] DecodedHeif decode_heif_file(const std::filesystem::path& path) {
    auto opened = open_primary_image(path);
    reject_unsupported_heif_layout(*opened.handle);
    HeifSourceColor source_color = source_color_from_handle(*opened.handle);

    heif_image* raw_image = nullptr;
    require_heif_success(
        heif_decode_image(
            opened.handle.get(),
            &raw_image,
            heif_colorspace_RGB,
            heif_chroma_interleaved_RGB,
            nullptr
        ),
        DecodeErrorCode::corrupt_data,
        "HEIF primary image could not be decoded as RGB"
    );
    HeifImagePtr image(raw_image);
    const int width = heif_image_get_width(image.get(), heif_channel_interleaved);
    const int height = heif_image_get_height(image.get(), heif_channel_interleaved);
    if (width <= 0 || height <= 0) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "HEIF RGB decode returned no dimensions");
    }
    const Dimensions dimensions{
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
    if (!dimensions_within_limits(dimensions)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "HEIF decoded dimensions exceed Shadow's retained raster limits"
        );
    }
    if (heif_image_get_bits_per_pixel_range(image.get(), heif_channel_interleaved) != 8) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "HEIF decode did not produce 8-bit SDR RGB samples"
        );
    }
    int source_stride = 0;
    const std::uint8_t* const source = heif_image_get_plane_readonly(
        image.get(),
        heif_channel_interleaved,
        &source_stride
    );
    const std::size_t packed_row_bytes = static_cast<std::size_t>(dimensions.width) * 3U;
    if (source == nullptr || source_stride <= 0
        || static_cast<std::size_t>(source_stride) < packed_row_bytes) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "HEIF RGB plane has an invalid stride"
        );
    }
    const std::uint64_t packed_bytes = dimensions.pixel_count() * 3U;
    if (packed_bytes > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "HEIF RGB allocation overflows");
    }
    DecodedHeif decoded;
    decoded.dimensions = dimensions;
    decoded.source_color = std::move(source_color);
    decoded.rgb.resize(static_cast<std::size_t>(packed_bytes));
    for (std::uint32_t row = 0U; row < dimensions.height; ++row) {
        const auto* const source_row = source + static_cast<std::size_t>(row)
            * static_cast<std::size_t>(source_stride);
        auto* const destination_row = decoded.rgb.data() + static_cast<std::size_t>(row)
            * packed_row_bytes;
        std::copy_n(source_row, packed_row_bytes, destination_row);
    }
    return decoded;
}

[[nodiscard]] PixelBuffer pixel_buffer_from_heif(const DecodedHeif& heif) {
    if (heif.rgb.empty() || !dimensions_within_limits(heif.dimensions)) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "HEIF decoder returned no RGB pixels");
    }
    const IccProfile source_profile = [&] {
        if (!heif.source_color.icc.empty()) {
            return load_icc_profile(std::as_bytes(std::span(heif.source_color.icc)));
        }
        switch (heif.source_color.transfer) {
        case HeifSourceColor::Transfer::display_srgb:
            return make_display_srgb_icc_profile();
        case HeifSourceColor::Transfer::display_rec709:
            return make_display_rec709_icc_profile();
        case HeifSourceColor::Transfer::linear_srgb:
            return make_linear_srgb_icc_profile();
        }
        throw DecodeError(DecodeErrorCode::internal, 0, "HEIF source transfer is unavailable");
    }();
    const IccProfile destination_profile = make_linear_srgb_icc_profile();
    const IccTransform transform = make_icc_transform(
        source_profile,
        destination_profile,
        IccRenderingIntent::relative_colorimetric,
        true
    );

    const std::uint64_t sample_count = heif.dimensions.pixel_count() * 3U;
    if (sample_count > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "HEIF output allocation overflows");
    }
    PixelBuffer output;
    output.dimensions = heif.dimensions;
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes = static_cast<std::size_t>(heif.dimensions.width) * 3U
        * sizeof(std::uint16_t);
    output.primaries = RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = RgbTransferFunction::linear;
    output.reference = RgbBufferReference::decoded_raster;
    output.samples.resize(static_cast<std::size_t>(sample_count));

    const std::size_t width = heif.dimensions.width;
    std::vector<float> transformed_row(width * 3U);
    for (std::uint32_t row = 0U; row < heif.dimensions.height; ++row) {
        const std::size_t row_offset = static_cast<std::size_t>(row) * width * 3U;
        for (std::size_t index = 0U; index < width * 3U; ++index) {
            transformed_row[index] = static_cast<float>(
                static_cast<double>(heif.rgb[row_offset + index]) / 255.0
            );
        }
        transform.apply_interleaved_rgb(transformed_row);
        for (std::size_t index = 0U; index < width * 3U; ++index) {
            output.samples[row_offset + index] = static_cast<std::uint16_t>(std::clamp(
                std::llround(std::clamp(
                    static_cast<double>(transformed_row[index]),
                    0.0,
                    1.0
                ) * 65'535.0),
                0LL,
                65'535LL
            ));
        }
    }
    return output;
}

class HeifSession final : public DecodeSession {
public:
    explicit HeifSession(std::filesystem::path path) : path_(std::move(path)) {
        auto opened = open_primary_image(path_);
        metadata_ = metadata_from_handle(*opened.handle);
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
        throw DecodeError(DecodeErrorCode::no_preview, 0, "HEIF has no selected embedded preview");
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "HEIF is a rendered raster and has no sensor RAW frame"
        );
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return pixel_buffer_from_heif(decode_heif_file(path_));
    }

private:
    std::filesystem::path path_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
};

} // namespace

#endif

bool heif_decoder_available() noexcept {
#if SHADOW_IMAGE_HAS_LIBHEIF
    try {
        ensure_heif_runtime();
        return heif_have_decoder_for_format(heif_compression_HEVC) != 0;
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

std::string heif_decoder_version() {
#if SHADOW_IMAGE_HAS_LIBHEIF
    std::ostringstream version;
    version << "libheif-" << heif_get_version() << ";contract="
            << heif_decoder_contract_version << ";hevc="
            << (heif_decoder_available() ? "available" : "unavailable");
    return version.str();
#else
    return "disabled";
#endif
}

std::unique_ptr<DecodeSession> open_heif_decode_session(const std::filesystem::path& path) {
#if SHADOW_IMAGE_HAS_LIBHEIF
    return std::make_unique<HeifSession>(path);
#else
    static_cast<void>(path);
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "HEIF/HEIC decoding is unavailable in this build; install libheif and rebuild Shadow"
    );
#endif
}

} // namespace shadow::image
