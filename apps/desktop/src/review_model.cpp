#include "review_model.hpp"

#include <QUrl>
#include <QVariant>

#include <utility>

ReviewModel::ReviewModel(QObject* parent) : QAbstractListModel(parent) {}

int ReviewModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(items_.size());
}

QVariant ReviewModel::data(const QModelIndex& index, const int role) const {
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
        return item.has_visual ? QString{} : QStringLiteral("visual pending");
    case VisualWidthRole:
        return QVariant::fromValue(item.visual_width);
    case VisualHeightRole:
        return QVariant::fromValue(item.visual_height);
    case VisualSourceRole:
        if (!item.has_visual) {
            return QString{};
        }
        return QStringLiteral("image://shadow/%1?generation=%2")
            .arg(item.representation_id)
            .arg(generation_.load(std::memory_order_relaxed));
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

void ReviewModel::replace(QVector<ReviewItem> items, const quint64 generation) {
    beginResetModel();
    items_ = std::move(items);
    generation_.store(generation, std::memory_order_release);
    endResetModel();
}

void ReviewModel::append(QVector<ReviewItem> items) {
    if (items.isEmpty()) {
        return;
    }
    const auto first = items_.size();
    const auto last = first + items.size() - 1;
    beginInsertRows({}, static_cast<int>(first), static_cast<int>(last));
    items_.append(std::move(items));
    endInsertRows();
}

bool ReviewModel::isGenerationCurrent(const quint64 generation) const noexcept {
    return generation_.load(std::memory_order_acquire) == generation;
}
