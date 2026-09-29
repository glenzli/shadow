#pragma once

#include "backend/export_settings_codec.hpp"

class QImage;
class QIODevice;

/// Writes one already transformed and sized output image to the selected
/// photographic file format. JPEG/PNG and 8-bit TIFF accept RGB888. A 16-bit
/// TIFF requires Format_RGBX64 so an upstream RGB8 raster cannot be silently
/// expanded and mislabeled as high-bit output. TIFF is owned directly through
/// libtiff so output capability does not depend on an optional Qt image plugin.
void writeEncodedOutputRaster(
    QIODevice& destination,
    const QImage& image,
    const BackendExportOptions& options
);
