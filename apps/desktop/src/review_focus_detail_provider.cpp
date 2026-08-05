#include "review_focus_detail_provider.hpp"

#include <QReadLocker>
#include <QUrlQuery>
#include <QWriteLocker>

#include <utility>

void ReviewFocusDetailStore::publish(const quint64 generation, QImage image) {
    QWriteLocker locker(&lock_);
    generation_ = generation;
    image_ = std::move(image);
}

void ReviewFocusDetailStore::clear(const quint64 generation) {
    QWriteLocker locker(&lock_);
    generation_ = generation;
    image_ = {};
}

QImage ReviewFocusDetailStore::snapshot(const quint64 generation) const {
    QReadLocker locker(&lock_);
    return generation == generation_ ? image_ : QImage{};
}

ReviewFocusDetailProvider::ReviewFocusDetailProvider(
    std::shared_ptr<ReviewFocusDetailStore> store
) :
    QQuickImageProvider(
        QQuickImageProvider::Image,
        QQmlImageProviderBase::ForceAsynchronousImageLoading
    ),
    store_(std::move(store)) {}

QImage ReviewFocusDetailProvider::requestImage(
    const QString& id,
    QSize* const size,
    const QSize& requested_size
) {
    Q_UNUSED(requested_size)
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    bool valid_generation = false;
    const quint64 generation =
        query.queryItemValue(QStringLiteral("generation")).toULongLong(&valid_generation);
    QImage image = valid_generation && store_ ? store_->snapshot(generation) : QImage{};
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}
