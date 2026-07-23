#include "thumbnail_provider.hpp"

#include "desktop_backend.hpp"
#include "review_model.hpp"

#include <QByteArrayView>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDebug>
#include <QImageReader>
#include <QMutexLocker>
#include <QUrlQuery>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace {

[[nodiscard]] std::uint32_t unsigned_dimension(const int value) noexcept {
    return value > 0 ? static_cast<std::uint32_t>(value) : 0;
}

[[nodiscard]] QString rgba8888_hash(const QImage& image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qsizetype row_bytes = static_cast<qsizetype>(image.width()) * 4;
    for (int row = 0; row < image.height(); ++row) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char*>(image.constScanLine(row)),
            row_bytes
        ));
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace

ThumbnailProvider::ThumbnailProvider(
    std::shared_ptr<DesktopBackend> backend,
    const ReviewModel* model
)
    : QQuickImageProvider(
          QQuickImageProvider::Image,
          QQmlImageProviderBase::ForceAsynchronousImageLoading
      ),
      backend_(std::move(backend)),
      model_(model) {
    // A 1024px RGBA preview is roughly 4–8 MiB. 192 MiB keeps dozens of
    // recently visible thumbnails warm without competing with edit previews.
    decoded_image_cache_.setMaxCost(192 * 1024);
}

QString ThumbnailProvider::cacheKey(
    const QString& ticket,
    const quint64 generation,
    const QSize& requested_size
) {
    return ticket + QLatin1Char(':') + QString::number(generation)
        + QLatin1Char(':') + QString::number(requested_size.width())
        + QLatin1Char('x') + QString::number(requested_size.height());
}

int ThumbnailProvider::imageCacheCost(const QImage& image) noexcept {
    const qsizetype bytes = image.sizeInBytes();
    const qsizetype kibibytes = std::max<qsizetype>(1, bytes / 1024);
    return kibibytes > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max() : static_cast<int>(kibibytes);
}

QImage ThumbnailProvider::requestImage(
    const QString& id,
    QSize* size,
    const QSize& requested_size
) {
    if (size != nullptr) {
        *size = {};
    }
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QString resource = query_start >= 0 ? id.left(query_start) : id;
    if (resource != QStringLiteral("visual")) {
        return {};
    }
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    const auto generation_values = query.allQueryItemValues(
        QStringLiteral("generation"),
        QUrl::FullyDecoded
    );
    const auto ticket_values = query.allQueryItemValues(
        QStringLiteral("ticket"),
        QUrl::FullyDecoded
    );
    if (generation_values.size() != 1 || ticket_values.size() != 1
        || ticket_values.constFirst().isEmpty()) {
        return {};
    }
    bool valid_generation = false;
    const quint64 generation = generation_values.constFirst().toULongLong(&valid_generation);
    if (!valid_generation || !model_->isGenerationCurrent(generation)) {
        return {};
    }
    const QString& ticket = ticket_values.constFirst();
    const QString cache_key = cacheKey(ticket, generation, requested_size);
    {
        const QMutexLocker lock(&decoded_image_cache_mutex_);
        if (const QImage* const cached = decoded_image_cache_.object(cache_key);
            cached != nullptr) {
            if (size != nullptr) {
                *size = cached->size();
            }
            return *cached;
        }
    }

    BackendReviewVisual payload;
    try {
        payload = backend_->loadReviewVisual(ticket);
    } catch (const std::exception& error) {
        qWarning() << "Cannot load Review visual" << ticket << error.what();
        return {};
    }
    if (!model_->isGenerationCurrent(generation)) {
        return {};
    }
    if (payload.bytes.isEmpty()) {
        return {};
    }

    QBuffer buffer;
    buffer.setData(payload.bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize original_size = reader.size();
    if (requested_size.isValid() && original_size.isValid()) {
        reader.setScaledSize(original_size.scaled(requested_size, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (image.isNull() || !model_->isGenerationCurrent(generation)) {
        return {};
    }

    if (payload.requires_frame_receipt) {
        image = image.convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            return {};
        }
        try {
            backend_->reportReviewVisualFrame(
                ticket,
                QString::fromLatin1(qVersion()),
                unsigned_dimension(requested_size.width()),
                unsigned_dimension(requested_size.height()),
                unsigned_dimension(image.width()),
                unsigned_dimension(image.height()),
                rgba8888_hash(image)
            );
        } catch (const std::exception& error) {
            qWarning() << "Cannot record Review visual frame" << ticket << error.what();
            return {};
        }
        if (!model_->isGenerationCurrent(generation)) {
            return {};
        }
    }

    if (size != nullptr) {
        *size = image.size();
    }
    {
        const QMutexLocker lock(&decoded_image_cache_mutex_);
        decoded_image_cache_.insert(
            cache_key,
            new QImage(image),
            imageCacheCost(image)
        );
    }
    return image;
}
