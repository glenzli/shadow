#include "png_decoder.hpp"

#include <shadow/image/color_management.hpp>
#include <shadow/image/decoder_error.hpp>

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>

namespace shadow::image {
namespace {
constexpr std::uintmax_t maximum_file_bytes = 64U * 1024U * 1024U;
constexpr std::uint64_t maximum_pixels = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_dimension = 16'384U;

[[noreturn]] void fail(const DecodeErrorCode code, const char* message) {
    throw DecodeError(code, 0, message);
}

std::uint32_t integer(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24U) | (std::uint32_t(p[1]) << 16U) | (std::uint32_t(p[2]) << 8U)
           | std::uint32_t(p[3]);
}

std::vector<std::uint8_t> read_source(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        fail(DecodeErrorCode::io, "Cannot read PNG source");
    if (size > maximum_file_bytes)
        fail(DecodeErrorCode::resource_limit, "PNG source exceeds the 64 MiB limit");
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))
        || stream.peek() != std::char_traits<char>::eof())
        fail(DecodeErrorCode::io, "PNG source changed or could not be read completely");
    return bytes;
}

Dimensions validate_header(const std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || !std::equal(signature.begin(), signature.end(), bytes.begin()))
        fail(DecodeErrorCode::corrupt_data, "Invalid PNG signature or header");
    Dimensions dimensions{};
    bool header = false, image_data = false, data_closed = false, ended = false;
    bool srgb = false, palette = false;
    std::size_t chunks = 0;
    for (std::size_t offset = signature.size(); offset < bytes.size();) {
        if (++chunks > 16384)
            fail(DecodeErrorCode::resource_limit, "PNG contains too many chunks");
        if (bytes.size() - offset < 12)
            fail(DecodeErrorCode::corrupt_data, "Truncated PNG chunk");
        const auto length = integer(bytes.data() + offset);
        if (length > bytes.size() - offset - 12)
            fail(DecodeErrorCode::corrupt_data, "Truncated PNG chunk payload");
        const auto checksum = crc32(0, bytes.data() + offset + 4, length + 4);
        if (checksum != integer(bytes.data() + offset + 8 + length))
            fail(DecodeErrorCode::corrupt_data, "PNG chunk checksum mismatch");
        const std::string_view type(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4);
        for (const char letter : type)
            if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
                fail(DecodeErrorCode::corrupt_data, "Invalid PNG chunk type");
        if (type[2] < 'A' || type[2] > 'Z')
            fail(DecodeErrorCode::corrupt_data, "Invalid PNG reserved chunk bit");
        if (image_data && type != "IDAT")
            data_closed = true;
        const auto* data = bytes.data() + offset + 8;
        if (!header && type != "IHDR")
            fail(DecodeErrorCode::corrupt_data, "PNG must begin with IHDR");
        if (type == "IHDR") {
            if (header || length != 13)
                fail(DecodeErrorCode::corrupt_data, "Invalid PNG IHDR");
            dimensions = {integer(data), integer(data + 4)};
            if (!dimensions.width || !dimensions.height || dimensions.width > maximum_dimension
                || dimensions.height > maximum_dimension
                || dimensions.pixel_count() > maximum_pixels)
                fail(
                    DecodeErrorCode::resource_limit,
                    "PNG dimensions exceed the 16-megapixel limit"
                );
            if (data[8] != 8 || (data[9] != PNG_COLOR_TYPE_RGB && data[9] != PNG_COLOR_TYPE_RGBA))
                fail(
                    DecodeErrorCode::unsupported_layout,
                    "PNG input requires 8-bit RGB or opaque RGBA"
                );
            if (data[10] != 0 || data[11] != 0 || data[12] > 1)
                fail(DecodeErrorCode::corrupt_data, "Invalid PNG encoding method");
            header = true;
        } else if (
            type == "iCCP" || type == "cICP" || type == "gAMA" || type == "cHRM" || type == "eXIf"
            || type == "tRNS" || type == "mDCV" || type == "cLLI"
        ) {
            fail(
                DecodeErrorCode::unsupported_layout,
                "PNG input requires opaque sRGB pixels without ICC, gamma, chromaticity or EXIF "
                "or HDR overrides"
            );
        } else if (type == "acTL" || type == "fcTL" || type == "fdAT") {
            fail(DecodeErrorCode::unsupported_layout, "Animated PNG input is not supported");
        } else if (type == "zTXt" || type == "iTXt") {
            fail(DecodeErrorCode::unsupported_layout, "Compressed PNG metadata is not supported");
        } else if (type == "sRGB") {
            if (srgb || image_data || palette || length != 1 || data[0] > 3)
                fail(DecodeErrorCode::corrupt_data, "Invalid or misplaced PNG sRGB declaration");
            srgb = true;
        } else if (type == "PLTE") {
            if (palette || image_data || length == 0 || length > 768 || length % 3 != 0)
                fail(DecodeErrorCode::corrupt_data, "Invalid or misplaced PNG palette");
            palette = true;
        } else if (type == "IDAT") {
            if (data_closed)
                fail(DecodeErrorCode::corrupt_data, "PNG IDAT chunks must be consecutive");
            image_data = true;
        } else if (type == "IEND") {
            if (length != 0 || !image_data || offset + 12 != bytes.size())
                fail(DecodeErrorCode::corrupt_data, "Invalid PNG end or trailing payload");
            ended = true;
        } else if (type[0] >= 'A' && type[0] <= 'Z') {
            fail(DecodeErrorCode::unsupported_layout, "Unknown PNG critical chunk");
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    if (!ended)
        fail(DecodeErrorCode::corrupt_data, "PNG is missing IEND");
    return dimensions;
}

struct PngImage final {
    png_image value{};
    PngImage() {
        value.version = PNG_IMAGE_VERSION;
    }
    ~PngImage() {
        png_image_free(&value);
    }
};

class Session final : public DecodeSession {
  public:
    explicit Session(const std::filesystem::path& path) : encoded_(read_source(path)) {
        metadata_.image_dimensions = validate_header(encoded_);
        metadata_.orientation = 1;
        metadata_.cfa_pattern = "none/rendered-raster";
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
    }
    const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }
    const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    std::span<const PreviewDescriptor> previews() const noexcept override {
        return {};
    }
    PreviewPayload decode_preview(std::size_t) override {
        fail(DecodeErrorCode::no_preview, "PNG has no separate embedded preview");
    }
    RawFrame decode_raw_frame() override {
        fail(DecodeErrorCode::unsupported_layout, "PNG has no sensor RAW frame");
    }
    PixelBuffer render_reference_rgb() const override {
        PngImage decoder;
        auto& image = decoder.value;
        if (!png_image_begin_read_from_memory(&image, encoded_.data(), encoded_.size())
            || image.warning_or_error != 0)
            fail(DecodeErrorCode::corrupt_data, "PNG header decoding failed");
        if ((image.flags & PNG_IMAGE_FLAG_COLORSPACE_NOT_sRGB) != 0)
            fail(DecodeErrorCode::unsupported_layout, "PNG primaries must be sRGB");
        // Preserve display-encoded bytes. The installed libpng's linear mode
        // uses a gamma approximation for direct RGB reads; use Shadow's exact
        // sRGB ICC transform, as the other rendered-raster decoders do.
        image.format = PNG_FORMAT_RGBA;
        PixelBuffer output;
        output.dimensions = metadata_.image_dimensions;
        if (image.width != output.dimensions.width || image.height != output.dimensions.height)
            fail(DecodeErrorCode::corrupt_data, "PNG decoded dimensions changed");
        const auto pixels = static_cast<std::size_t>(output.dimensions.pixel_count());
        std::vector<std::uint8_t> rgba(pixels * 4);
        if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)
            || image.warning_or_error != 0)
            fail(DecodeErrorCode::corrupt_data, "PNG pixels or chunk integrity are invalid");
        // A separable sRGB transfer has just 256 possible inputs. Build the
        // lookup through the common color owner once, with no per-pixel ICC work.
        static const auto linear = [] {
            std::array<float, 256 * 3> values{};
            for (std::size_t i = 0; i < 256; ++i)
                for (std::size_t c = 0; c < 3; ++c)
                    values[i * 3 + c] = static_cast<float>(i) / 255.0F;
            const auto transform =
                make_icc_transform(make_display_srgb_icc_profile(), make_linear_srgb_icc_profile());
            transform.apply_interleaved_rgb(values);
            std::array<std::uint16_t, 256> result{};
            for (std::size_t i = 0; i < 256; ++i)
                result[i] = static_cast<std::uint16_t>(
                    std::lround(std::clamp(values[i * 3], 0.0F, 1.0F) * 65535.0F)
                );
            return result;
        }();
        output.samples.resize(pixels * 3);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (rgba[i * 4 + 3] != 255)
                fail(DecodeErrorCode::unsupported_layout, "Transparent PNG input is not supported");
            for (std::size_t channel = 0; channel < 3; ++channel)
                output.samples[i * 3 + channel] = linear[rgba[i * 4 + channel]];
        }
        output.bits_per_channel = 16;
        output.channels = 3;
        output.row_stride_bytes = static_cast<std::size_t>(output.dimensions.width) * 6;
        output.primaries = RgbPrimaries::srgb_rec709_d65;
        output.transfer_function = RgbTransferFunction::linear;
        output.reference = RgbBufferReference::decoded_raster;
        return output;
    }

  private:
    // One immutable encoded source per opened decoder, capped at 64 MiB. Ordinary
    // gestures reuse the existing warm source; this does not retain pixel history.
    std::vector<std::uint8_t> encoded_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
};
} // namespace

std::unique_ptr<DecodeSession> open_png_decode_session(const std::filesystem::path& path) {
    return std::make_unique<Session>(path);
}
std::string png_decoder_version() {
    return "libpng-" PNG_LIBPNG_VER_STRING ";opaque-srgb8-v1";
}
} // namespace shadow::image
