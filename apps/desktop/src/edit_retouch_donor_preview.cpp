#include "edit_retouch_donor_preview.hpp"
#include "edit_preview_provider.hpp"
#include <QImage>

std::optional<EditRetouchDonorSelection> preview_retouch_source_selection(
    const std::shared_ptr<EditPreviewStore>& store,
    const QString& revision,
    QSize level_zero,
    std::span<const QPointF> points,
    double radius,
    int mode
) {
    bool valid = false;
    const auto generation = revision.toULongLong(&valid);
    if (!valid || !store)
        return std::nullopt;
    const auto snapshot = store->snapshot(EditPreviewSlot::Current, generation);
    if (snapshot.dimensions.isEmpty() || snapshot.dimensions.width() > 4096
        || snapshot.dimensions.height() > 4096)
        return std::nullopt;
    QImage decoded;
    std::span<const std::uint8_t> pixels;
    auto stride = snapshot.row_stride_bytes;
    try {
        if (snapshot.frame)
            pixels = snapshot.frame->materializeRgb8();
        else if (stride > 0)
            pixels = {
                reinterpret_cast<const std::uint8_t*>(snapshot.bytes.constData()),
                static_cast<std::size_t>(snapshot.bytes.size())
            };
        else {
            decoded = QImage::fromData(snapshot.bytes).convertToFormat(QImage::Format_RGB888);
            if (decoded.isNull() || decoded.size() != snapshot.dimensions)
                return std::nullopt;
            stride = decoded.bytesPerLine();
            pixels = {decoded.constBits(), static_cast<std::size_t>(decoded.sizeInBytes())};
        }
    } catch (...) {
        return std::nullopt;
    }
    if (stride <= 0)
        return std::nullopt;
    return select_edit_retouch_donor(
        {.preview_rgb8 = pixels,
         .preview_dimensions = snapshot.dimensions,
         .preview_row_stride_bytes = static_cast<std::size_t>(stride),
         .level_zero_dimensions = level_zero,
         .normalized_target_points = points,
         .radius_level_zero_pixels = radius,
         .mode = mode == 1 ? EditRetouchDonorMode::Clone : EditRetouchDonorMode::Heal}
    );
}
