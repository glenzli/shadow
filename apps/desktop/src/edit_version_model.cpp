#include "edit_version_model.hpp"

#include "edit_version_presentation.hpp"

#include <QDateTime>
#include <QVariant>

#include <utility>

EditVersionModel::EditVersionModel(QObject* parent) : QAbstractListModel(parent) {}

int EditVersionModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(versions_.size());
}

QVariant EditVersionModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= versions_.size()) {
        return {};
    }
    const auto& version = versions_.at(index.row());
    switch (role) {
    case CommitIdRole:
        return version.commit_id;
    case LabelRole:
        return EditVersionPresentation::displayName(version);
    case CreatedAtTextRole:
        return QDateTime::fromMSecsSinceEpoch(version.created_at_ms)
            .toLocalTime()
            .toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss"));
    case CurrentRole:
        return version.is_working;
    case ParentCountRole:
        return version.parent_commit_ids.size();
    case ChangeSummaryRole:
        return EditVersionPresentation::changeSummary(version);
    case ParentSummaryRole:
        return EditVersionPresentation::parentSummary(version);
    default:
        return {};
    }
}

QHash<int, QByteArray> EditVersionModel::roleNames() const {
    return {
        {CommitIdRole, "commitId"},
        {LabelRole, "label"},
        {CreatedAtTextRole, "createdAtText"},
        {CurrentRole, "current"},
        {ParentCountRole, "parentCount"},
        {ChangeSummaryRole, "changeSummary"},
        {ParentSummaryRole, "parentSummary"},
    };
}

void EditVersionModel::replace(QVector<BackendEditVersion> versions) {
    beginResetModel();
    versions_ = std::move(versions);
    endResetModel();
}
