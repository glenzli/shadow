#pragma once

#include <QQuickImageProvider>

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
    std::shared_ptr<DesktopBackend> backend_;
    const ReviewModel* model_;
};
