#include "embedded_preview.hpp"
#include "../src/proxy/jpeg_proxy_encoding.hpp"

#include <shadow/image/decoder.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

constexpr std::uint64_t maximum_bytes = 64U * 1024U * 1024U;
constexpr std::uint32_t maximum_side = 16'384U;

void encode_camera_bitmap(shadow::image::PreviewPayload& payload, const std::int32_t orientation) {
    namespace image = shadow::image;
    auto& descriptor = payload.descriptor;
    if ((descriptor.bits_per_channel != 8U && descriptor.bits_per_channel != 16U)
        || (descriptor.channels != 1U && descriptor.channels != 3U)
        || descriptor.dimensions.pixel_count() > maximum_bytes / 3U
        || (orientation != 0 && orientation != 3 && orientation != 5 && orientation != 6)) {
        throw std::runtime_error("embedded bitmap is outside the bounded camera-preview contract");
    }
    const auto bytes_per_sample = descriptor.bits_per_channel / 8U;
    const auto expected =
        descriptor.dimensions.pixel_count() * descriptor.channels * bytes_per_sample;
    if (payload.bytes.size() != expected) {
        throw std::runtime_error("embedded bitmap has an invalid byte layout");
    }
    if (bytes_per_sample == 2U && payload.byte_order != image::ByteOrder::native
        && payload.byte_order != image::ByteOrder::little_endian
        && payload.byte_order != image::ByteOrder::big_endian) {
        throw std::runtime_error("16-bit camera preview requires an explicit byte order");
    }
    const bool little_endian = payload.byte_order == image::ByteOrder::little_endian
                               || (payload.byte_order == image::ByteOrder::native
                                   && std::endian::native == std::endian::little);
    // LibRaw preview descriptors are already in display geometry; bitmap bytes remain in the
    // stored geometry. Normalize only the camera visual, without invoking RAW development.
    const bool transpose = orientation == 5 || orientation == 6;
    const auto stored_width =
        transpose ? descriptor.dimensions.height : descriptor.dimensions.width;
    const auto stored_height =
        transpose ? descriptor.dimensions.width : descriptor.dimensions.height;
    std::vector<std::uint8_t> rgb(
        static_cast<std::size_t>(descriptor.dimensions.pixel_count() * 3U)
    );
    for (std::uint32_t y = 0U; y < descriptor.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < descriptor.dimensions.width; ++x) {
            auto sx = transpose ? y : x;
            auto sy = transpose ? x : y;
            if (orientation == 3 || orientation == 5) {
                sx = stored_width - 1U - sx;
            }
            if (orientation == 3 || orientation == 6) {
                sy = stored_height - 1U - sy;
            }
            const auto source_pixel = static_cast<std::uint64_t>(sy) * stored_width + sx;
            const auto destination_pixel =
                static_cast<std::uint64_t>(y) * descriptor.dimensions.width + x;
            for (std::uint16_t channel = 0U; channel < 3U; ++channel) {
                const auto source_channel = descriptor.channels == 1U ? 0U : channel;
                const auto offset = static_cast<std::size_t>(
                    (source_pixel * descriptor.channels + source_channel) * bytes_per_sample
                );
                std::uint16_t value = payload.bytes[offset];
                if (bytes_per_sample == 2U) {
                    value =
                        little_endian
                            ? static_cast<std::uint16_t>(
                                  value
                                  | (static_cast<std::uint16_t>(payload.bytes[offset + 1U]) << 8U)
                              )
                            : static_cast<std::uint16_t>(
                                  (value << 8U) | payload.bytes[offset + 1U]
                              );
                    value = static_cast<std::uint16_t>(
                        (static_cast<std::uint32_t>(value) + 128U) / 257U
                    );
                }
                rgb[static_cast<std::size_t>(destination_pixel * 3U + channel)] =
                    static_cast<std::uint8_t>(value);
            }
        }
    }
    payload.bytes = image::proxy_detail::encode_proxy_jpeg(rgb, descriptor.dimensions, 90U);
    descriptor.format = image::PreviewFormat::jpeg;
    descriptor.bits_per_channel = 8U;
    descriptor.channels = 3U;
    descriptor.encoded_bytes = payload.bytes.size();
    payload.byte_order = image::ByteOrder::not_applicable;
}

} // namespace

int extract_embedded_preview(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const std::string_view nonce
) {
    namespace image = shadow::image;
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    return write_embedded_preview(*session, output, nonce);
}

int write_embedded_preview(
    shadow::image::DecodeSession& session,
    const std::filesystem::path& output,
    const std::string_view nonce
) {
    namespace image = shadow::image;
    const auto previews = session.previews();
    const image::PreviewDescriptor* selected = nullptr;
    for (const auto& preview : previews) {
        if (!preview.decodable
            || (preview.format != image::PreviewFormat::jpeg
                && preview.format != image::PreviewFormat::bitmap)
            || preview.dimensions.width == 0U || preview.dimensions.height == 0U
            || std::max(preview.dimensions.width, preview.dimensions.height) > maximum_side
            || preview.encoded_bytes > maximum_bytes) {
            continue;
        }
        const auto area =
            static_cast<std::uint64_t>(preview.dimensions.width) * preview.dimensions.height;
        if (selected == nullptr
            || area > static_cast<std::uint64_t>(selected->dimensions.width)
                          * selected->dimensions.height) {
            selected = &preview;
        }
    }
    if (selected == nullptr) {
        std::cout << "shadow-embedded-preview-v1 " << nonce << " none\n";
        return 0;
    }
    auto payload = session.decode_preview(selected->id);
    if (payload.descriptor.format == image::PreviewFormat::bitmap) {
        encode_camera_bitmap(payload, session.metadata().orientation);
    }
    const auto& descriptor = payload.descriptor;
    if (descriptor.format != image::PreviewFormat::jpeg || payload.bytes.size() < 4U
        || payload.bytes.size() > maximum_bytes || descriptor.dimensions.width == 0U
        || descriptor.dimensions.height == 0U
        || std::max(descriptor.dimensions.width, descriptor.dimensions.height) > maximum_side
        || descriptor.bits_per_channel != 8U
        || (descriptor.channels != 1U && descriptor.channels != 3U) || payload.bytes[0] != 0xffU
        || payload.bytes[1] != 0xd8U || payload.bytes[payload.bytes.size() - 2U] != 0xffU
        || payload.bytes.back() != 0xd9U) {
        throw std::runtime_error("embedded preview is outside the bounded JPEG contract");
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream.write(
        reinterpret_cast<const char*>(payload.bytes.data()),
        static_cast<std::streamsize>(payload.bytes.size())
    );
    stream.close();
    if (!stream) {
        throw std::runtime_error("cannot write embedded preview output");
    }
    std::cout << "shadow-embedded-preview-v1 " << nonce << " jpeg " << descriptor.id << ' '
              << descriptor.dimensions.width << ' ' << descriptor.dimensions.height << ' '
              << descriptor.bits_per_channel << ' ' << descriptor.channels << ' '
              << payload.bytes.size() << '\n';
    return 0;
}
