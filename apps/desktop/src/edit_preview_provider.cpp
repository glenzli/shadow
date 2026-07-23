#include "edit_preview_provider.hpp"

#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QReadLocker>
#include <QUrlQuery>
#include <QWriteLocker>

#include <memory>
#include <utility>

namespace {

void release_detail_pixels(void* const owner) noexcept {
    delete static_cast<QByteArray*>(owner);
}

} // namespace

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
    QImage display_zebra,
    const quint64 generation
) {
    QWriteLocker lock(&lock_);
    auto& stored = slot(target);
    stored.bytes = std::move(bytes);
    stored.dimensions = dimensions;
    stored.display_zebra = std::move(display_zebra);
    stored.generation = generation;
}

void EditPreviewStore::clear(
    const EditPreviewSlot target,
    const quint64 generation
) {
    QWriteLocker lock(&lock_);
    auto& stored = slot(target);
    stored = {};
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
        .row_stride_bytes = stored.row_stride_bytes,
        .display_zebra = stored.display_zebra,
    };
}

void EditPreviewStore::publishDetails(
    QVector<DetailPublication> publications,
    const EditDetailGeneration generation
) {
    QWriteLocker lock(&lock_);
    details_.clear();
    detail_generation_ = generation;
    for (auto& publication : publications) {
        if (publication.ticket.isEmpty() || publication.bytes.isEmpty()) {
            continue;
        }
        details_.insert(
            publication.ticket,
            StoredPreview{
                .bytes = std::move(publication.bytes),
                .dimensions = publication.dimensions,
                .generation = 0,
                .row_stride_bytes = publication.row_stride_bytes,
            }
        );
    }
}

void EditPreviewStore::clearDetails(const EditDetailGeneration generation) {
    QWriteLocker lock(&lock_);
    details_.clear();
    detail_generation_ = generation;
}

EditPreviewStore::Snapshot EditPreviewStore::detailSnapshot(
    const QString& ticket,
    const EditDetailGeneration generation
) const {
    QReadLocker lock(&lock_);
    if (generation != detail_generation_) {
        return {};
    }
    const auto found = details_.constFind(ticket);
    if (found == details_.cend()) {
        return {};
    }
    return {
        .bytes = found->bytes,
        .dimensions = found->dimensions,
        .row_stride_bytes = found->row_stride_bytes,
        .display_zebra = found->display_zebra,
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
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    if (slot_name.startsWith(QStringLiteral("scope/"))) {
        const QStringList scope_parts = slot_name.split(QLatin1Char('/'));
        if (scope_parts.size() != 3 || scope_parts.at(1) != QStringLiteral("zebra")) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        EditPreviewSlot scope_slot;
        if (scope_parts.at(2) == QStringLiteral("current")) {
            scope_slot = EditPreviewSlot::Current;
        } else if (scope_parts.at(2) == QStringLiteral("before")) {
            scope_slot = EditPreviewSlot::Before;
        } else {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
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
        const auto snapshot = store_->snapshot(scope_slot, generation);
        const QImage scope_image = snapshot.display_zebra;
        if (scope_image.isNull()) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        if (size != nullptr) {
            *size = scope_image.size();
        }
        return scope_image;
    }
    if (slot_name.startsWith(QStringLiteral("detail/"))) {
        const QString ticket = slot_name.mid(7);
        bool valid_photo = false;
        bool valid_recipe = false;
        bool valid_viewport = false;
        const EditDetailGeneration generation{
            .photo = query.queryItemValue(QStringLiteral("photo")).toULongLong(
                &valid_photo
            ),
            .recipe_revision = query
                                   .queryItemValue(QStringLiteral("recipe"))
                                   .toULongLong(&valid_recipe),
            .viewport_revision = query
                                     .queryItemValue(QStringLiteral("viewport"))
                                     .toULongLong(&valid_viewport),
        };
        if (ticket.isEmpty() || !valid_photo || !valid_recipe || !valid_viewport) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        auto snapshot = store_->detailSnapshot(ticket, generation);
        const bool valid_dimensions = snapshot.dimensions.isValid();
        const quint64 minimum_stride = valid_dimensions
            ? static_cast<quint64>(snapshot.dimensions.width()) * 3U
            : 0U;
        const quint64 expected_bytes = snapshot.row_stride_bytes > 0
            && valid_dimensions
            ? static_cast<quint64>(snapshot.row_stride_bytes)
                * static_cast<quint64>(snapshot.dimensions.height())
            : 0U;
        const bool valid_layout = valid_dimensions
            && static_cast<quint64>(snapshot.row_stride_bytes) == minimum_stride
            && snapshot.row_stride_bytes > 0
            && expected_bytes == static_cast<quint64>(snapshot.bytes.size());
        if (snapshot.bytes.isEmpty() || !valid_layout) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        auto* const pixel_owner = new QByteArray(std::move(snapshot.bytes));
        QImage image(
            reinterpret_cast<const uchar*>(pixel_owner->constData()),
            snapshot.dimensions.width(),
            snapshot.dimensions.height(),
            snapshot.row_stride_bytes,
            QImage::Format_RGB888,
            release_detail_pixels,
            pixel_owner
        );
        image.setColorSpace(QColorSpace::SRgb);
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }
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
