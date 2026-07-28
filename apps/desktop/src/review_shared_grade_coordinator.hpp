#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

/// Owns Review's shared Grade Node snapshot and batch-link contract.
///
/// The coordinator normalizes QML selection targets into unique photo/source
/// pairs, performs the batch mutation, projects the complete receipt, publishes
/// localized status, and requests a Library refresh only when rows changed.
class ReviewSharedGradeCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<QVector<BackendSharedGradeNode>()> nodes;
        std::function<BackendBatchGradeReceipt(
            const QString& layer_id,
            const QVector<BackendBatchPhotoTarget>& targets
        )> apply;
    };

    explicit ReviewSharedGradeCoordinator(
        Operations operations,
        QObject* parent = nullptr
    );

    [[nodiscard]] QVariantList nodes() const;
    [[nodiscard]] LocalizedUiMessage statusMessage() const;

    void refresh();
    [[nodiscard]] QVariantMap apply(
        const QString& layer_id,
        const QVariantList& targets
    );

signals:
    void nodesChanged();
    void statusMessageChanged();
    void libraryRefreshRequested();

private:
    [[nodiscard]] static QVector<BackendBatchPhotoTarget> batchTargets(
        const QVariantList& targets
    );
    [[nodiscard]] static QVariantMap emptyReceipt();
    void publishStatus(LocalizedUiMessage status);

    Operations operations_;
    QVector<BackendSharedGradeNode> nodes_;
    LocalizedUiMessage status_message_;
};
