#pragma once

#include "backend/export_settings_codec.hpp"

class QImage;
class QIODevice;

/// Writes one already transformed and sized RGB8 output image to the selected
/// photographic file format. TIFF is owned directly through libtiff so output
/// capability does not depend on an optional Qt image plugin.
void writeEncodedOutputRaster(
    QIODevice& destination,
    const QImage& image,
    const BackendExportOptions& options
);
