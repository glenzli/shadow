#include "thumbnail_provider.hpp"

#include "desktop_backend.hpp"
#include "review_model.hpp"

#include <QBuffer>
#include <QDebug>
#include <QImageReader>
#include <QUrlQuery>

#include <algorithm>

ThumbnailProvider::ThumbnailProvider(
    std::shared_ptr<DesktopBackend> backend,
    const ReviewModel* model
)
    : QQuickImageProvider(
          QQuickImageProvider::Image,
          QQmlImageProviderBase::ForceAsynchronousImageLoading
      ),
      backend_(std::move(backend)),
      model_(model) {}

QImage ThumbnailProvider::requestImage(
    const QString& id,
    QSize* size,
    const QSize& requested_size
) {
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QString representation_id = id.left(query_start);
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    bool valid_generation = false;
    const quint64 generation = query
                                   .queryItemValue(QStringLiteral("generation"))
                                   .toULongLong(&valid_generation);
    if (!valid_generation || !model_->isGenerationCurrent(generation)) {
        return {};
    }

    QByteArray bytes;
    try {
        bytes = backend_->loadReviewVisual(representation_id);
    } catch (const std::exception& error) {
        qWarning() << "Cannot load Review visual" << representation_id << error.what();
        return {};
    }
    if (!model_->isGenerationCurrent(generation)) {
        return {};
    }
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
