#include "thumbnail_provider.hpp"

#include "review_model.hpp"

#include <QBuffer>
#include <QImageReader>

#include <algorithm>

ThumbnailProvider::ThumbnailProvider(const ReviewModel* model)
    : QQuickImageProvider(QQuickImageProvider::Image), model_(model) {}

QImage ThumbnailProvider::requestImage(
    const QString& id,
    QSize* size,
    const QSize& requested_size
) {
    const QString representation_id = id.section(QLatin1Char('?'), 0, 0);
    const QByteArray bytes = model_->visualBytes(representation_id);
    if (bytes.isEmpty()) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }

    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize original_size = reader.size();
    if (requested_size.isValid() && original_size.isValid()) {
        reader.setScaledSize(original_size.scaled(requested_size, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}
