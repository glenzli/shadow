#include "backend/export_raster_encoder.hpp"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QImageWriter>
#include <QIODevice>

#include <tiffio.h>

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

[[nodiscard]] QIODevice* device(const thandle_t handle) {
    return static_cast<QIODevice*>(handle);
}

tmsize_t read_tiff_bytes(
    const thandle_t handle,
    void* const bytes,
    const tmsize_t count
) {
    return count < 0 ? -1 : device(handle)->read(
        static_cast<char*>(bytes),
        static_cast<qint64>(count)
    );
}

tmsize_t write_tiff_bytes(
    const thandle_t handle,
    void* const bytes,
    const tmsize_t count
) {
    return count < 0 ? -1 : device(handle)->write(
        static_cast<const char*>(bytes),
        static_cast<qint64>(count)
    );
}

toff_t seek_tiff_bytes(
    const thandle_t handle,
    const toff_t offset,
    const int origin
) {
    QIODevice* const output = device(handle);
    qint64 position = 0;
    if (origin == SEEK_SET) {
        position = static_cast<qint64>(offset);
    } else if (origin == SEEK_CUR) {
        position = output->pos() + static_cast<qint64>(offset);
    } else if (origin == SEEK_END) {
        position = output->size() + static_cast<qint64>(offset);
    } else {
        return static_cast<toff_t>(-1);
    }
    if (position < 0 || !output->seek(position)) {
        return static_cast<toff_t>(-1);
    }
    return static_cast<toff_t>(output->pos());
}

int close_tiff_bytes(thandle_t) {
    return 0;
}

toff_t size_tiff_bytes(const thandle_t handle) {
    return static_cast<toff_t>(device(handle)->size());
}

int map_tiff_bytes(thandle_t, void**, toff_t*) {
    return 0;
}

void unmap_tiff_bytes(thandle_t, void*, toff_t) {}

void require_tiff_field(const int accepted, const char* const name) {
    if (accepted != 1) {
        throw std::runtime_error(
            std::string("could not configure TIFF field: ") + name
        );
    }
}

void write_tiff(
    QIODevice& destination,
    const QImage& input,
    const BackendExportOptions& options
) {
    QImage image = input.convertToFormat(QImage::Format_RGB888);
    if (image.isNull()) {
        throw std::runtime_error("could not prepare the TIFF RGB8 raster");
    }
    TIFF* const tiff = TIFFClientOpen(
        "Shadow output",
        "w",
        static_cast<thandle_t>(&destination),
        read_tiff_bytes,
        write_tiff_bytes,
        seek_tiff_bytes,
        close_tiff_bytes,
        size_tiff_bytes,
        map_tiff_bytes,
        unmap_tiff_bytes
    );
    if (tiff == nullptr) {
        throw std::runtime_error("could not initialize the TIFF encoder");
    }
    try {
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, image.width()),
            "width"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, image.height()),
            "height"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 3),
            "samples per pixel"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8),
            "bits per sample"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT),
            "orientation"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            "planar configuration"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
            "photometric interpretation"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE),
            "compression"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL),
            "predictor"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH),
            "resolution unit"
        );
        const float resolution = static_cast<float>(options.resolution_dpi);
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_XRESOLUTION, resolution),
            "horizontal resolution"
        );
        require_tiff_field(
            TIFFSetField(tiff, TIFFTAG_YRESOLUTION, resolution),
            "vertical resolution"
        );
        const QByteArray profile = image.colorSpace().iccProfile();
        if (!profile.isEmpty()
            && profile.size()
                <= static_cast<qsizetype>(
                    std::numeric_limits<std::uint32_t>::max()
                )) {
            require_tiff_field(
                TIFFSetField(
                    tiff,
                    TIFFTAG_ICCPROFILE,
                    static_cast<std::uint32_t>(profile.size()),
                    profile.constData()
                ),
                "ICC profile"
            );
        }
        const QByteArray creator = options.creator.toUtf8();
        const QByteArray copyright = options.copyright_notice.toUtf8();
        if (options.metadata_policy == QStringLiteral("copyright-only")) {
            if (!creator.isEmpty()) {
                require_tiff_field(
                    TIFFSetField(tiff, TIFFTAG_ARTIST, creator.constData()),
                    "artist"
                );
            }
            if (!copyright.isEmpty()) {
                require_tiff_field(
                    TIFFSetField(
                        tiff,
                        TIFFTAG_COPYRIGHT,
                        copyright.constData()
                    ),
                    "copyright"
                );
            }
        }
        for (int row = 0; row < image.height(); ++row) {
            if (TIFFWriteScanline(
                    tiff,
                    const_cast<uchar*>(image.constScanLine(row)),
                    static_cast<std::uint32_t>(row),
                    0
                ) < 0) {
                throw std::runtime_error("could not encode a TIFF scanline");
            }
        }
        TIFFClose(tiff);
    } catch (...) {
        TIFFClose(tiff);
        throw;
    }
}

} // namespace

void writeEncodedOutputRaster(
    QIODevice& destination,
    const QImage& image,
    const BackendExportOptions& options
) {
    if (options.format == QStringLiteral("tiff")) {
        write_tiff(destination, image, options);
        return;
    }
    const QByteArray encoder = options.format == QStringLiteral("png")
        ? QByteArrayLiteral("png")
        : QByteArrayLiteral("jpg");
    QImageWriter writer(&destination, encoder);
    if (options.format == QStringLiteral("jpeg")) {
        writer.setQuality(options.jpeg_quality);
        writer.setOptimizedWrite(true);
    }
    if (options.metadata_policy == QStringLiteral("copyright-only")) {
        if (!options.creator.isEmpty()) {
            writer.setText(QStringLiteral("Author"), options.creator);
        }
        if (!options.copyright_notice.isEmpty()) {
            writer.setText(
                QStringLiteral("Copyright"),
                options.copyright_notice
            );
        }
    }
    if (!writer.write(image)) {
        throw std::runtime_error(
            std::string("could not encode export: ")
            + writer.errorString().toStdString()
        );
    }
}
