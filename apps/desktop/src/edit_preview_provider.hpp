#pragma once

#include "edit_preview_contract.hpp"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QReadWriteLock>
#include <QSize>
#include <QString>
#include <QVector>

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
        qsizetype row_stride_bytes = 0;
        QImage display_zebra;
        QImage luma_waveform;
    };

    struct DetailPublication final {
        QString ticket;
        QByteArray bytes;
        QSize dimensions;
        qsizetype row_stride_bytes = 0;
    };

    void publish(
        EditPreviewSlot slot,
        QByteArray bytes,
        QSize dimensions,
        QImage display_zebra,
        QImage luma_waveform,
        quint64 generation
    );
    void clear(EditPreviewSlot slot, quint64 generation);
    void clearAll(quint64 current_generation, quint64 before_generation);
    [[nodiscard]] Snapshot snapshot(EditPreviewSlot slot, quint64 generation) const;
    void publishDetails(
        QVector<DetailPublication> publications,
        EditDetailGeneration generation
    );
    void clearDetails(EditDetailGeneration generation);
    [[nodiscard]] Snapshot detailSnapshot(
        const QString& ticket,
        EditDetailGeneration generation
    ) const;

private:
    struct StoredPreview final {
        QByteArray bytes;
        QSize dimensions;
        quint64 generation = 0;
        qsizetype row_stride_bytes = 0;
        QImage display_zebra;
        QImage luma_waveform;
    };

    [[nodiscard]] StoredPreview& slot(EditPreviewSlot slot) noexcept;
    [[nodiscard]] const StoredPreview& slot(EditPreviewSlot slot) const noexcept;

    mutable QReadWriteLock lock_;
    StoredPreview current_;
    StoredPreview before_;
    QHash<QString, StoredPreview> details_;
    EditDetailGeneration detail_generation_;
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
