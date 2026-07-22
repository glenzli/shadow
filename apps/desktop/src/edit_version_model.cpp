#include "edit_version_model.hpp"

#include "edit_version_presentation.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QVariant>

#include <utility>

EditVersionModel::EditVersionModel(QObject* parent) : QAbstractListModel(parent) {
  if (auto *const application = QCoreApplication::instance()) {
    application->installEventFilter(this);
  }
}

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
    case SelectedRole:
        return version.is_selected;
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
        {SelectedRole, "selected"},
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

void EditVersionModel::setSelectedCommit(const QString& commit_id) {
    bool changed = false;
    for (auto& version : versions_) {
        const bool selected = !commit_id.isEmpty() && version.commit_id == commit_id;
        if (version.is_selected != selected) {
            version.is_selected = selected;
            changed = true;
        }
    }
    if (changed && !versions_.isEmpty()) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SelectedRole});
    }
}

void EditVersionModel::retranslateUi() {
  if (versions_.isEmpty()) {
    return;
  }
  emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                   {LabelRole, ChangeSummaryRole, ParentSummaryRole});
}

bool EditVersionModel::eventFilter(QObject *const watched,
                                   QEvent *const event) {
  if (watched == QCoreApplication::instance() &&
      event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  return QAbstractListModel::eventFilter(watched, event);
}
