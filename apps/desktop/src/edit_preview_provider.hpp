#pragma once

#include <QByteArray>
#include <QQuickImageProvider>
#include <QReadWriteLock>
#include <QSize>

#include <memory>

class EditPreviewStore final {
public:
    struct Snapshot final {
        QByteArray bytes;
        QSize dimensions;
    };

    void publish(QByteArray bytes, QSize dimensions, quint64 generation);
    void clear(quint64 generation);
    [[nodiscard]] Snapshot snapshot(quint64 generation) const;

private:
    mutable QReadWriteLock lock_;
    QByteArray bytes_;
    QSize dimensions_;
    quint64 generation_ = 0;
};

class EditPreviewProvider final : public QQuickImageProvider {
public:
    explicit EditPreviewProvider(std::shared_ptr<EditPreviewStore> store);

    [[nodiscard]] QImage requestImage(
        const QString& id,
        QSize* size,
        const QSize& requested_size
    ) override;

private:
    std::shared_ptr<EditPreviewStore> store_;
};
