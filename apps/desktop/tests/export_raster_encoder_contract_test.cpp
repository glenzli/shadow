#include "backend/export_raster_encoder.hpp"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <tiffio.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary output root is available")) {
        return EXIT_FAILURE;
    }
    const QString path = root.filePath(QStringLiteral("output.tif"));
    QFile destination(path);
    if (!require(destination.open(QIODevice::WriteOnly), "TIFF output opens for writing")) {
        return EXIT_FAILURE;
    }
    QImage image(7, 5, QImage::Format_RGB888);
    image.fill(QColor(24, 96, 180));
    image.setColorSpace(QColorSpace::SRgb);
    BackendExportOptions options;
    options.format = QStringLiteral("tiff");
    options.resolution_dpi = 360;
    options.metadata_policy = QStringLiteral("copyright-only");
    options.creator = QStringLiteral("Shadow Photographer");
    options.copyright_notice = QStringLiteral("Copyright Shadow");
    writeEncodedOutputRaster(destination, image, options);
    destination.close();

    TIFF* const tiff = TIFFOpen(path.toUtf8().constData(), "r");
    if (!require(tiff != nullptr, "libtiff reopens the encoded output")) {
        return EXIT_FAILURE;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t samples = 0;
    std::uint16_t bits = 0;
    std::uint16_t compression = 0;
    float x_resolution = 0.0F;
    char* artist = nullptr;
    char* copyright = nullptr;
    std::uint32_t profile_length = 0;
    void* profile = nullptr;
    const bool valid =
        require(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 1, "TIFF width tag exists")
        && require(TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 1, "TIFF height tag exists")
        && require(
            TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples) == 1,
            "TIFF sample count exists"
        )
        && require(TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits) == 1, "TIFF bit depth exists")
        && require(
            TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression) == 1,
            "TIFF compression exists"
        )
        && require(
            TIFFGetField(tiff, TIFFTAG_XRESOLUTION, &x_resolution) == 1,
            "TIFF resolution exists"
        )
        && require(TIFFGetField(tiff, TIFFTAG_ARTIST, &artist) == 1, "TIFF creator exists")
        && require(TIFFGetField(tiff, TIFFTAG_COPYRIGHT, &copyright) == 1, "TIFF copyright exists")
        && require(
            TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &profile_length, &profile) == 1,
            "TIFF ICC profile exists"
        )
        && require(width == 7U && height == 5U, "TIFF dimensions match the output raster")
        && require(samples == 3U && bits == 8U, "TIFF honestly records RGB8")
        && require(
            compression == COMPRESSION_ADOBE_DEFLATE,
            "TIFF uses lossless Deflate compression"
        )
        && require(
            std::abs(x_resolution - 360.0F) < 0.01F,
            "TIFF records the requested print resolution"
        )
        && require(
            artist != nullptr && std::string(artist) == "Shadow Photographer",
            "TIFF records only the requested creator"
        )
        && require(
            copyright != nullptr && std::string(copyright) == "Copyright Shadow",
            "TIFF records only the requested copyright"
        )
        && require(
            profile_length > 0U && profile != nullptr,
            "TIFF embeds an output color profile"
        );
    TIFFClose(tiff);

    const QString high_bit_path = root.filePath(QStringLiteral("output-16.tif"));
    QFile high_bit_destination(high_bit_path);
    if (!require(
            high_bit_destination.open(QIODevice::WriteOnly),
            "16-bit TIFF output opens for writing"
        )) {
        return EXIT_FAILURE;
    }
    QImage high_bit_image(7, 5, QImage::Format_RGBX64);
    for (int row = 0; row < high_bit_image.height(); ++row) {
        auto* const pixels = reinterpret_cast<QRgba64*>(high_bit_image.scanLine(row));
        for (int column = 0; column < high_bit_image.width(); ++column) {
            pixels[column] = QRgba64::fromRgba64(
                static_cast<std::uint16_t>(12'345 + column),
                static_cast<std::uint16_t>(34'567 + row),
                60'000,
                65'535
            );
        }
    }
    high_bit_image.setColorSpace(QColorSpace::DisplayP3);
    BackendExportOptions high_bit_options = options;
    high_bit_options.tiff_bit_depth = 16;
    writeEncodedOutputRaster(high_bit_destination, high_bit_image, high_bit_options);
    high_bit_destination.close();

    TIFF* const high_bit_tiff = TIFFOpen(high_bit_path.toUtf8().constData(), "r");
    if (!require(high_bit_tiff != nullptr, "libtiff reopens the RGB16 output")) {
        return EXIT_FAILURE;
    }
    std::uint16_t high_bit_bits = 0;
    std::uint32_t high_bit_profile_length = 0;
    void* high_bit_profile = nullptr;
    std::vector<std::uint16_t> scanline(7U * 3U);
    const bool high_bit_valid =
        require(
            TIFFGetField(high_bit_tiff, TIFFTAG_BITSPERSAMPLE, &high_bit_bits) == 1,
            "RGB16 TIFF bit depth exists"
        )
        && require(high_bit_bits == 16U, "TIFF honestly records RGB16")
        && require(
            TIFFGetField(
                high_bit_tiff,
                TIFFTAG_ICCPROFILE,
                &high_bit_profile_length,
                &high_bit_profile
            ) == 1,
            "RGB16 TIFF ICC profile exists"
        )
        && require(
            high_bit_profile_length > 0U && high_bit_profile != nullptr,
            "RGB16 TIFF embeds its Display P3 profile"
        )
        && require(
            TIFFReadScanline(high_bit_tiff, scanline.data(), 0, 0) == 1,
            "RGB16 TIFF scanline decodes"
        )
        && require(
            scanline[0] == 12'345U && scanline[1] == 34'567U && scanline[2] == 60'000U,
            "RGB16 TIFF preserves non-expanded high-bit channel values"
        );
    TIFFClose(high_bit_tiff);

    bool rejected_fake_high_bit = false;
    try {
        QFile rejected_destination(root.filePath(QStringLiteral("fake-16.tif")));
        static_cast<void>(rejected_destination.open(QIODevice::WriteOnly));
        writeEncodedOutputRaster(rejected_destination, image, high_bit_options);
    } catch (const std::invalid_argument&) {
        rejected_fake_high_bit = true;
    }
    return valid && high_bit_valid
                   && require(
                       rejected_fake_high_bit,
                       "RGB8 input cannot be relabeled as a 16-bit TIFF"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
