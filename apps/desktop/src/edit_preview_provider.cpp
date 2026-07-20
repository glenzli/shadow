#include "edit_preview_provider.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QReadLocker>
#include <QUrlQuery>
#include <QWriteLocker>

#include <utility>

void EditPreviewStore::publish(
    QByteArray bytes,
    const QSize dimensions,
    const quint64 generation
) {
    QWriteLocker lock(&lock_);
    bytes_ = std::move(bytes);
    dimensions_ = dimensions;
    generation_ = generation;
}

void EditPreviewStore::clear(const quint64 generation) {
    QWriteLocker lock(&lock_);
    bytes_.clear();
    dimensions_ = {};
    generation_ = generation;
}

EditPreviewStore::Snapshot EditPreviewStore::snapshot(const quint64 generation) const {
    QReadLocker lock(&lock_);
    if (generation != generation_) {
        return {};
    }
    return {
        .bytes = bytes_,
        .dimensions = dimensions_,
    };
}

EditPreviewProvider::EditPreviewProvider(std::shared_ptr<EditPreviewStore> store)
    : QQuickImageProvider(
          QQuickImageProvider::Image,
          QQmlImageProviderBase::ForceAsynchronousImageLoading
      ),
      store_(std::move(store)) {}

QImage EditPreviewProvider::requestImage(
    const QString& id,
    QSize* size,
    const QSize& requested_size
) {
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    bool valid_generation = false;
    const quint64 generation = query
                                   .queryItemValue(QStringLiteral("generation"))
                                   .toULongLong(&valid_generation);
    if (!valid_generation) {
        return {};
    }
    const auto snapshot = store_->snapshot(generation);
    if (snapshot.bytes.isEmpty()) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }

    QBuffer buffer;
    buffer.setData(snapshot.bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize original_size = reader.size().isValid() ? reader.size() : snapshot.dimensions;
    if (requested_size.isValid() && original_size.isValid()) {
        reader.setScaledSize(original_size.scaled(requested_size, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}
