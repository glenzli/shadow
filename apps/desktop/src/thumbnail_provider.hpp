#pragma once

#include <QQuickImageProvider>

class ReviewModel;

class ThumbnailProvider final : public QQuickImageProvider {
public:
    explicit ThumbnailProvider(const ReviewModel* model);

    [[nodiscard]] QImage requestImage(
        const QString& id,
        QSize* size,
        const QSize& requested_size
    ) override;

private:
    const ReviewModel* model_;
};
