#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QReadWriteLock>

#include <memory>

/// Bounded publication store for the one focus-detail image visible in Review.
///
/// The store deliberately retains no history: changing the selected photo
/// invalidates the prior generation before another level-zero region is
/// published.
class ReviewFocusDetailStore final {
  public:
    void publish(quint64 generation, QImage image);
    void clear(quint64 generation);
    [[nodiscard]] QImage snapshot(quint64 generation) const;

  private:
    mutable QReadWriteLock lock_;
    quint64 generation_ = 0;
    QImage image_;
};

class ReviewFocusDetailProvider final : public QQuickImageProvider {
  public:
    explicit ReviewFocusDetailProvider(std::shared_ptr<ReviewFocusDetailStore> store);

    [[nodiscard]] QImage
    requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

  private:
    std::shared_ptr<ReviewFocusDetailStore> store_;
};
