#include "edit_preview_provider.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QReadLocker>
#include <QUrlQuery>
#include <QWriteLocker>

#include <utility>

EditPreviewStore::StoredPreview& EditPreviewStore::slot(
    const EditPreviewSlot slot
) noexcept {
    return slot == EditPreviewSlot::Before ? before_ : current_;
}

const EditPreviewStore::StoredPreview& EditPreviewStore::slot(
    const EditPreviewSlot slot
) const noexcept {
    return slot == EditPreviewSlot::Before ? before_ : current_;
}

void EditPreviewStore::publish(
    const EditPreviewSlot target,
    QByteArray bytes,
    const QSize dimensions,
    const quint64 generation
) {
    QWriteLocker lock(&lock_);
    auto& stored = slot(target);
    stored.bytes = std::move(bytes);
    stored.dimensions = dimensions;
    stored.generation = generation;
}

void EditPreviewStore::clear(
    const EditPreviewSlot target,
    const quint64 generation
) {
    QWriteLocker lock(&lock_);
    auto& stored = slot(target);
    stored.bytes.clear();
    stored.dimensions = {};
    stored.generation = generation;
}

void EditPreviewStore::clearAll(
    const quint64 current_generation,
    const quint64 before_generation
) {
    QWriteLocker lock(&lock_);
    current_ = {};
    current_.generation = current_generation;
    before_ = {};
    before_.generation = before_generation;
}

EditPreviewStore::Snapshot EditPreviewStore::snapshot(
    const EditPreviewSlot target,
    const quint64 generation
) const {
    QReadLocker lock(&lock_);
    const auto& stored = slot(target);
    if (generation != stored.generation) {
        return {};
    }
    return {
        .bytes = stored.bytes,
        .dimensions = stored.dimensions,
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
    const QString slot_name = query_start >= 0 ? id.left(query_start) : id;
    EditPreviewSlot slot;
    if (slot_name == QStringLiteral("current")) {
        slot = EditPreviewSlot::Current;
    } else if (slot_name == QStringLiteral("before")) {
        slot = EditPreviewSlot::Before;
    } else {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    bool valid_generation = false;
    const quint64 generation = query
                                   .queryItemValue(QStringLiteral("generation"))
                                   .toULongLong(&valid_generation);
    if (!valid_generation) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    const auto snapshot = store_->snapshot(slot, generation);
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
