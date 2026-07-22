#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

/// Lazily renders the bundled neutral reference photograph through managed
/// `.cube` resources. Preview files are content addressed by the LUT bytes,
/// reference pixels, and renderer version, so a changed LUT cannot reuse a
/// stale thumbnail.
class LutPreviewProvider final : public QQuickImageProvider {
public:
    LutPreviewProvider(
        QString managed_store_root,
        QString preview_cache_root,
        QString reference_path = QStringLiteral(
            ":/assets/lut-preview-reference.jpg"
        )
    );

    [[nodiscard]] QImage requestImage(
        const QString& id,
        QSize* size,
        const QSize& requested_size
    ) override;

private:
    [[nodiscard]] QImage scaledForRequest(
        const QImage& image,
        const QSize& requested_size
    ) const;
    [[nodiscard]] QString cachePath(const QString& content_id) const;

    QString managed_store_root_;
    QString preview_cache_root_;
    QString reference_hash_;
    QImage reference_;
    QMutex cache_mutex_;
};
