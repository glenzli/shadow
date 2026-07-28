#include "review_shared_grade_coordinator.hpp"

#include <QSet>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage shared_grade_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewSharedGradeCoordinator::ReviewSharedGradeCoordinator(
    Operations operations,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)) {
    if (!operations_.nodes || !operations_.apply) {
        throw std::invalid_argument(
            "all Review shared Grade Node operations are required"
        );
    }
}

QVariantList ReviewSharedGradeCoordinator::nodes() const {
    QVariantList result;
    result.reserve(nodes_.size());
    for (const auto& shared : nodes_) {
        result.push_back(QVariantMap{
            {QStringLiteral("layerId"), shared.layer_id},
            {QStringLiteral("revisionId"), shared.revision_id},
            {QStringLiteral("revisionNumber"), shared.revision_number},
            {QStringLiteral("label"), shared.label},
        });
    }
    return result;
}

LocalizedUiMessage ReviewSharedGradeCoordinator::statusMessage() const {
    return status_message_;
}

void ReviewSharedGradeCoordinator::refresh() {
    try {
        auto refreshed = operations_.nodes();
        if (refreshed != nodes_) {
            nodes_ = std::move(refreshed);
            emit nodesChanged();
        }
    } catch (const std::exception& error) {
        publishStatus(shared_grade_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not load shared Grade Nodes · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
    }
}

QVariantMap ReviewSharedGradeCoordinator::apply(
    const QString& layer_id,
    const QVariantList& targets
) {
    const QVector<BackendBatchPhotoTarget> batch = batchTargets(targets);
    if (layer_id.isEmpty() || batch.isEmpty()) {
        return emptyReceipt();
    }
    try {
        const BackendBatchGradeReceipt receipt =
            operations_.apply(layer_id, batch);
        QStringList errors;
        errors.reserve(receipt.errors.size());
        for (const auto& error : receipt.errors) {
            errors.push_back(error);
        }
        if (receipt.failed == 0) {
            publishStatus(shared_grade_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Shared Grade Node linked to %1 photos · %2 already current"
                ),
                {
                    static_cast<qulonglong>(receipt.updated),
                    static_cast<qulonglong>(receipt.unchanged),
                }
            ));
        } else {
            publishStatus(shared_grade_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Shared Grade Node linked to %1 photos · %2 failed"
                ),
                {
                    static_cast<qulonglong>(receipt.updated),
                    static_cast<qulonglong>(receipt.failed),
                }
            ));
        }
        if (receipt.updated > 0) {
            emit libraryRefreshRequested();
        }
        return {
            {QStringLiteral("requested"), receipt.requested},
            {QStringLiteral("updated"), receipt.updated},
            {QStringLiteral("unchanged"), receipt.unchanged},
            {QStringLiteral("failed"), receipt.failed},
            {QStringLiteral("errors"), errors},
        };
    } catch (const std::exception& error) {
        const QString message = QString::fromUtf8(error.what());
        publishStatus(shared_grade_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not apply shared Grade Node · %1"
            ),
            {message}
        ));
        return {
            {QStringLiteral("requested"), batch.size()},
            {QStringLiteral("updated"), 0},
            {QStringLiteral("unchanged"), 0},
            {QStringLiteral("failed"), batch.size()},
            {QStringLiteral("errors"), QStringList{message}},
        };
    }
}

QVector<BackendBatchPhotoTarget>
ReviewSharedGradeCoordinator::batchTargets(const QVariantList& targets) {
    QVector<BackendBatchPhotoTarget> batch;
    batch.reserve(targets.size());
    QSet<QString> seen_photo_ids;
    for (const auto& value : targets) {
        const QVariantMap target = value.toMap();
        const QString photo_id =
            target.value(QStringLiteral("photoId")).toString();
        const QString source_path =
            target.value(QStringLiteral("sourcePath")).toString();
        if (photo_id.isEmpty() || source_path.isEmpty()
            || seen_photo_ids.contains(photo_id)) {
            continue;
        }
        seen_photo_ids.insert(photo_id);
        batch.push_back({
            .photo_id = photo_id,
            .source_path = source_path,
        });
    }
    return batch;
}

QVariantMap ReviewSharedGradeCoordinator::emptyReceipt() {
    return {
        {QStringLiteral("requested"), 0},
        {QStringLiteral("updated"), 0},
        {QStringLiteral("unchanged"), 0},
        {QStringLiteral("failed"), 0},
        {QStringLiteral("errors"), QStringList{}},
    };
}

void ReviewSharedGradeCoordinator::publishStatus(
    LocalizedUiMessage status
) {
    status_message_ = std::move(status);
    emit statusMessageChanged();
}
