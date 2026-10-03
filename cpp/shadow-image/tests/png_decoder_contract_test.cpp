#include "contract_test_assertions.hpp"

#include <png.h>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/edited_proxy_rendering.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <vector>

namespace image = shadow::image;
namespace {
using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using Bytes = std::vector<unsigned char>;

struct Directory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / ("shadow-png-contract-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() {
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("fixture directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

Bytes encode(
    const bool transparent = false,
    const bool grayscale = false,
    const bool rgb_only = false
) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = 3;
    image.height = 1;
    image.format = grayscale ? PNG_FORMAT_GRAY : rgb_only ? PNG_FORMAT_RGB : PNG_FORMAT_RGBA;
    std::array<unsigned char, 12> pixels{0, 128, 255, 255, 255, 0, 128, 255, 128, 255, 0, 255};
    if (transparent)
        pixels[7] = 128;
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, pixels.data(), 0, nullptr))
        throw std::runtime_error("measure fixture PNG");
    Bytes bytes(size);
    if (!png_image_write_to_memory(&image, bytes.data(), &size, 0, pixels.data(), 0, nullptr))
        throw std::runtime_error("encode fixture PNG");
    bytes.resize(size);
    png_image_free(&image);
    return bytes;
}
void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!file)
        throw std::runtime_error("write fixture PNG");
}
void integer(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<unsigned char>(value >> ((3 - i) * 8));
}
Bytes chunk(const std::array<unsigned char, 4>& type, const Bytes& payload) {
    Bytes out(payload.size() + 12);
    integer(out, 0, static_cast<std::uint32_t>(payload.size()));
    std::copy(type.begin(), type.end(), out.begin() + 4);
    std::copy(payload.begin(), payload.end(), out.begin() + 8);
    integer(
        out,
        out.size() - 4,
        static_cast<std::uint32_t>(crc32(0, out.data() + 4, static_cast<uInt>(out.size() - 8)))
    );
    return out;
}
void rejected(const std::filesystem::path& path, image::DecodeErrorCode expected, const char* why) {
    bool rejected = false;
    try {
        const auto decoder = image::make_photo_decoder_provider(std::filesystem::path{});
        const auto opened = decoder->open(path);
        (void)opened->render_reference_rgb();
    } catch (const image::DecodeError& error) {
        rejected = error.code() == expected;
    }
    expect(rejected, why);
}
void contracts() {
    Directory directory;
    const auto path = directory.path / "sample.png";
    const auto bytes = encode();
    write(path, bytes);
    const auto provider = image::make_photo_decoder_provider(std::filesystem::path{});
    const auto opened = provider->open(path);
    const auto rgb = opened->render_reference_rgb();
    expect(
        rgb.dimensions.width == 3 && rgb.dimensions.height == 1 && rgb.samples.size() == 9,
        "opaque RGBA PNG admits exact dimensions and three RGB channels"
    );
    expect(
        rgb.bits_per_channel == 16 && rgb.channels == 3
            && rgb.transfer_function == image::RgbTransferFunction::linear
            && rgb.reference == image::RgbBufferReference::decoded_raster
            && !rgb.raw_development_receipt.recorded(),
        "PNG enters the existing linear reference contract without RAW provenance"
    );
    expect(
        rgb.samples[0] == 0 && rgb.samples[2] == 65535 && rgb.samples[1] >= 14140
            && rgb.samples[1] <= 14152,
        "sRGB samples convert to the expected linear values"
    );
    expect(
        provider->info().version.find("raster=") != std::string::npos,
        "provider identity incorporates the raster implementation revision"
    );
    const image::ProxyRequest request{.max_edge = 1024, .jpeg_quality = 90};
    const std::array nodes{
        image::AdjustmentNode{.node_id = "neutral", .parameters = image::ExposureAdjustment{}}
    };
    const auto proxy = image::render_reference_proxy_jpeg(*opened, request);
    const auto edited = image::render_edited_reference_proxy_jpeg(*opened, nodes, request);
    expect(
        proxy.bytes == edited.bytes,
        "PNG neutral edit and reference display share exact pixels"
    );
    const auto warm = image::prepare_warm_edit_preview(*opened, 1024);
    std::stop_source cancel;
    cancel.request_stop();
    expect(
        warm.render_jpeg_cancellable(nodes, 90, cancel.get_token()).cancelled(),
        "PNG source keeps existing warm preview cancellation"
    );
    expect(!warm.render_jpeg(nodes, 90).bytes.empty(), "cancel does not poison PNG warm preview");

    // RGB without alpha and without a color tag is explicitly assumed sRGB.
    auto untagged = encode(false, false, true);
    for (std::size_t offset = 8; offset + 12 <= untagged.size();) {
        const auto length = (std::uint32_t(untagged[offset]) << 24)
                            | (std::uint32_t(untagged[offset + 1]) << 16)
                            | (std::uint32_t(untagged[offset + 2]) << 8) | untagged[offset + 3];
        if (std::equal(untagged.data() + offset + 4, untagged.data() + offset + 8, "sRGB")) {
            untagged.erase(
                untagged.begin() + static_cast<std::ptrdiff_t>(offset),
                untagged.begin() + static_cast<std::ptrdiff_t>(offset + length + 12)
            );
            break;
        }
        offset += length + 12;
    }
    write(path, untagged);
    const auto untagged_rgb = provider->open(path)->render_reference_rgb();
    expect(
        untagged_rgb.samples[1] == rgb.samples[1],
        "untagged RGB uses the same exact sRGB transfer"
    );

    for (const bool after_data : {false, true}) {
        auto invalid = bytes;
        const auto extra = chunk({'s', 'R', 'G', 'B'}, {0});
        invalid.insert(
            after_data ? invalid.end() - 12 : invalid.begin() + 33,
            extra.begin(),
            extra.end()
        );
        write(path, invalid);
        rejected(
            path,
            image::DecodeErrorCode::corrupt_data,
            "duplicate or post-IDAT sRGB cannot alter color identity"
        );
    }
    auto invalid = bytes;
    const auto critical = chunk({'A', 'B', 'C', 'D'}, {});
    invalid.insert(invalid.end() - 12, critical.begin(), critical.end());
    write(path, invalid);
    rejected(
        path,
        image::DecodeErrorCode::unsupported_layout,
        "unknown critical chunk after valid pixels is rejected"
    );
    invalid = bytes;
    const auto ancillary = chunk({'t', 'E', 'X', 't'}, {'k', 0, 'v'});
    const auto data_tail = chunk({'I', 'D', 'A', 'T'}, {});
    invalid.insert(invalid.end() - 12, ancillary.begin(), ancillary.end());
    invalid.insert(invalid.end() - 12, data_tail.begin(), data_tail.end());
    write(path, invalid);
    rejected(path, image::DecodeErrorCode::corrupt_data, "non-consecutive IDAT is rejected");
    // Valid 24-byte HDR mastering metadata, with BT.2020 chromaticities.
    const Bytes mastering{0x8a, 0x48, 0x39, 0x08, 0x21, 0x34, 0x9b, 0xaa, 0x19, 0x96, 0x08, 0xfc,
                          0x3d, 0x13, 0x40, 0x42, 0,    0x98, 0x96, 0x80, 0,    0,    0,    1};
    const auto hdr = chunk({'m', 'D', 'C', 'V'}, mastering);
    invalid = bytes;
    invalid.insert(invalid.begin() + 33, hdr.begin(), hdr.end());
    write(path, invalid);
    rejected(
        path,
        image::DecodeErrorCode::unsupported_layout,
        "valid HDR mastering metadata is explicit unsupported"
    );
    const Bytes text{'b', 'o', 'u', 'n', 'd', 'e', 'd'};
    Bytes compressed(compressBound(text.size()));
    uLongf size = compressed.size();
    if (compress(compressed.data(), &size, text.data(), text.size()) != Z_OK)
        throw std::runtime_error("compress tiny fixture");
    compressed.resize(size);
    compressed.insert(compressed.begin(), {'k', 0, 0});
    const auto zipped = chunk({'z', 'T', 'X', 't'}, compressed);
    invalid = bytes;
    invalid.insert(invalid.begin() + 33, zipped.begin(), zipped.end());
    write(path, invalid);
    rejected(
        path,
        image::DecodeErrorCode::unsupported_layout,
        "compressed metadata is refused before decompression"
    );

    write(path, encode(true));
    rejected(
        path,
        image::DecodeErrorCode::unsupported_layout,
        "transparency is rejected instead of flattened"
    );
    write(path, encode(false, true));
    rejected(path, image::DecodeErrorCode::unsupported_layout, "unsupported grayscale is explicit");
    auto corrupt = bytes;
    corrupt.back() ^= 1;
    write(path, corrupt);
    rejected(path, image::DecodeErrorCode::corrupt_data, "IEND CRC cannot be silently skipped");
    corrupt = bytes;
    corrupt.resize(corrupt.size() - 4);
    write(path, corrupt);
    rejected(path, image::DecodeErrorCode::corrupt_data, "truncated PNG is rejected");
    corrupt = bytes;
    integer(corrupt, 16, 16385);
    integer(corrupt, 29, static_cast<std::uint32_t>(crc32(0, corrupt.data() + 12, 17)));
    write(path, corrupt);
    rejected(
        path,
        image::DecodeErrorCode::resource_limit,
        "dimensions fail before pixel allocation"
    );
    for (const auto& type :
         {std::array<unsigned char, 4>{'i', 'C', 'C', 'P'},
          {'a', 'c', 'T', 'L'},
          {'e', 'X', 'I', 'f'},
          {'g', 'A', 'M', 'A'}}) {
        const auto extra = chunk(type, Bytes{0, 0, 0, 0});
        auto overridden = bytes;
        overridden.insert(overridden.begin() + 33, extra.begin(), extra.end());
        write(path, overridden);
        rejected(
            path,
            image::DecodeErrorCode::unsupported_layout,
            "profile, animation and orientation overrides are rejected"
        );
    }
    const auto oversized = directory.path / "oversized.png";
    {
        std::ofstream file(oversized, std::ios::binary);
        file.seekp(64 * 1024 * 1024);
        file.put('x');
    }
    rejected(
        oversized,
        image::DecodeErrorCode::resource_limit,
        "encoded input size is bounded before allocation"
    );
    expect(
        opened->render_reference_rgb().samples == rgb.samples,
        "an opened decoder retains its exact immutable bytes after external path replacement"
    );
}
} // namespace
int main() {
    try {
        contracts();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
