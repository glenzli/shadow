#include "backend/export_raster_encoder.hpp"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <tiffio.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

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
    if (!require(
            destination.open(QIODevice::WriteOnly),
            "TIFF output opens for writing"
        )) {
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
        require(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 1,
                "TIFF width tag exists")
        && require(TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 1,
                   "TIFF height tag exists")
        && require(TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples) == 1,
                   "TIFF sample count exists")
        && require(TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits) == 1,
                   "TIFF bit depth exists")
        && require(TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression) == 1,
                   "TIFF compression exists")
        && require(TIFFGetField(tiff, TIFFTAG_XRESOLUTION, &x_resolution) == 1,
                   "TIFF resolution exists")
        && require(TIFFGetField(tiff, TIFFTAG_ARTIST, &artist) == 1,
                   "TIFF creator exists")
        && require(TIFFGetField(tiff, TIFFTAG_COPYRIGHT, &copyright) == 1,
                   "TIFF copyright exists")
        && require(TIFFGetField(
                       tiff,
                       TIFFTAG_ICCPROFILE,
                       &profile_length,
                       &profile
                   ) == 1,
                   "TIFF ICC profile exists")
        && require(width == 7U && height == 5U,
                   "TIFF dimensions match the output raster")
        && require(samples == 3U && bits == 8U,
                   "TIFF honestly records RGB8")
        && require(compression == COMPRESSION_ADOBE_DEFLATE,
                   "TIFF uses lossless Deflate compression")
        && require(std::abs(x_resolution - 360.0F) < 0.01F,
                   "TIFF records the requested print resolution")
        && require(artist != nullptr
                       && std::string(artist) == "Shadow Photographer",
                   "TIFF records only the requested creator")
        && require(copyright != nullptr
                       && std::string(copyright) == "Copyright Shadow",
                   "TIFF records only the requested copyright")
        && require(profile_length > 0U && profile != nullptr,
                   "TIFF embeds an output color profile");
    TIFFClose(tiff);
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
