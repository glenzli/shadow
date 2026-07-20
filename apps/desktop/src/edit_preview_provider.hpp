#pragma once

#include <QByteArray>
#include <QQuickImageProvider>
#include <QReadWriteLock>
#include <QSize>

#include <cstdint>
#include <memory>

enum class EditPreviewSlot : std::uint8_t {
    Current,
    Before,
};

class EditPreviewStore final {
public:
    struct Snapshot final {
        QByteArray bytes;
        QSize dimensions;
    };

    void publish(
        EditPreviewSlot slot,
        QByteArray bytes,
        QSize dimensions,
        quint64 generation
    );
    void clear(EditPreviewSlot slot, quint64 generation);
    void clearAll(quint64 current_generation, quint64 before_generation);
    [[nodiscard]] Snapshot snapshot(EditPreviewSlot slot, quint64 generation) const;

private:
    struct StoredPreview final {
        QByteArray bytes;
        QSize dimensions;
        quint64 generation = 0;
    };

    [[nodiscard]] StoredPreview& slot(EditPreviewSlot slot) noexcept;
    [[nodiscard]] const StoredPreview& slot(EditPreviewSlot slot) const noexcept;

    mutable QReadWriteLock lock_;
    StoredPreview current_;
    StoredPreview before_;
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
