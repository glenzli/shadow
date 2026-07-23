#pragma once

#include <QQuickImageProvider>
#include <QCache>
#include <QMutex>

#include <memory>

class DesktopBackend;
class ReviewModel;

class ThumbnailProvider final : public QQuickImageProvider {
public:
    ThumbnailProvider(std::shared_ptr<DesktopBackend> backend, const ReviewModel* model);

    [[nodiscard]] QImage requestImage(
        const QString& id,
        QSize* size,
        const QSize& requested_size
    ) override;

private:
    [[nodiscard]] static QString cacheKey(
        const QString& ticket,
        quint64 generation,
        const QSize& requested_size
    );
    [[nodiscard]] static int imageCacheCost(const QImage& image) noexcept;

    std::shared_ptr<DesktopBackend> backend_;
    const ReviewModel* model_;
    // Qt can call requestImage concurrently. Keep an explicit decoded-image
    // LRU above the persistent JPEG proxy cache so gallery resizing/scrolling
    // never repeatedly decodes the same on-disk preview.
    QCache<QString, QImage> decoded_image_cache_;
    QMutex decoded_image_cache_mutex_;
};
