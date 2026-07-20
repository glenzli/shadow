#include "review_model.hpp"

#include <QReadLocker>
#include <QUrl>
#include <QVariant>
#include <QWriteLocker>

#include <utility>

ReviewModel::ReviewModel(QObject* parent) : QAbstractListModel(parent) {}

int ReviewModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    QReadLocker locker(&lock_);
    return static_cast<int>(items_.size());
}

QVariant ReviewModel::data(const QModelIndex& index, const int role) const {
    QReadLocker locker(&lock_);
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) {
        return {};
    }
    const auto& item = items_.at(index.row());
    switch (role) {
    case PhotoIdRole:
        return item.photo_id;
    case RepresentationIdRole:
        return item.representation_id;
    case TitleRole:
        return item.title;
    case SourcePathRole:
        return item.source_path;
    case VisualRole:
        return item.visual_role;
    case VisualErrorRole:
        return item.visual_error;
    case VisualWidthRole:
        return QVariant::fromValue(item.visual_width);
    case VisualHeightRole:
        return QVariant::fromValue(item.visual_height);
    case VisualSourceRole:
        if (item.visual_bytes.isEmpty()) {
            return QString{};
        }
        return QStringLiteral("image://shadow/%1?v=%2")
            .arg(item.representation_id)
            .arg(generation_);
    default:
        return {};
    }
}

QHash<int, QByteArray> ReviewModel::roleNames() const {
    return {
        {PhotoIdRole, "photoId"},
        {RepresentationIdRole, "representationId"},
        {TitleRole, "title"},
        {SourcePathRole, "sourcePath"},
        {VisualRole, "visualRole"},
        {VisualErrorRole, "visualError"},
        {VisualWidthRole, "visualWidth"},
        {VisualHeightRole, "visualHeight"},
        {VisualSourceRole, "visualSource"},
    };
}

void ReviewModel::replace(QVector<ReviewItem> items) {
    beginResetModel();
    {
        QWriteLocker locker(&lock_);
        items_ = std::move(items);
        row_by_id_.clear();
        for (qsizetype row = 0; row < items_.size(); ++row) {
            row_by_id_.insert(items_.at(row).representation_id, row);
        }
        ++generation_;
    }
    endResetModel();
}

QByteArray ReviewModel::visualBytes(const QString& representation_id) const {
    QReadLocker locker(&lock_);
    const auto found = row_by_id_.constFind(representation_id);
    if (found == row_by_id_.cend()) {
        return {};
    }
    return items_.at(found.value()).visual_bytes;
}
