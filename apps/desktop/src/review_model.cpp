#include "review_model.hpp"

#include <QUrl>
#include <QUrlQuery>
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
    case VisualHandleRole:
        return item.visual_handle;
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
        return visualSourceFor(item.visual_handle);
    case HasTechnicalObservationRole:
        return item.has_technical_observation;
    case TechnicalInputWidthRole:
        return QVariant::fromValue(item.technical_input_width);
    case TechnicalInputHeightRole:
        return QVariant::fromValue(item.technical_input_height);
    case TechnicalPreprocessingVersionRole:
        return item.technical_preprocessing_version;
    case TechnicalImplementationVersionRole:
        return item.technical_implementation_version;
    case MeanLumaRole:
        return item.mean_luma;
    case P01LumaRole:
        return item.p01_luma;
    case P50LumaRole:
        return item.p50_luma;
    case P99LumaRole:
        return item.p99_luma;
    case NearBlackFractionRole:
        return item.near_black_fraction;
    case NearWhiteFractionRole:
        return item.near_white_fraction;
    case LaplacianVarianceRole:
        return item.laplacian_variance;
    case EdgeEnergyRole:
        return item.edge_energy;
    default:
        return {};
    }
}

QHash<int, QByteArray> ReviewModel::roleNames() const {
    return {
        {PhotoIdRole, "photoId"},
        {RepresentationIdRole, "representationId"},
        {VisualHandleRole, "visualHandle"},
        {TitleRole, "title"},
        {SourcePathRole, "sourcePath"},
        {VisualRole, "visualRole"},
        {VisualErrorRole, "visualError"},
        {VisualWidthRole, "visualWidth"},
        {VisualHeightRole, "visualHeight"},
        {VisualSourceRole, "visualSource"},
        {HasTechnicalObservationRole, "hasTechnicalObservation"},
        {TechnicalInputWidthRole, "technicalInputWidth"},
        {TechnicalInputHeightRole, "technicalInputHeight"},
        {TechnicalPreprocessingVersionRole, "technicalPreprocessingVersion"},
        {TechnicalImplementationVersionRole, "technicalImplementationVersion"},
        {MeanLumaRole, "meanLuma"},
        {P01LumaRole, "p01Luma"},
        {P50LumaRole, "p50Luma"},
        {P99LumaRole, "p99Luma"},
        {NearBlackFractionRole, "nearBlackFraction"},
        {NearWhiteFractionRole, "nearWhiteFraction"},
        {LaplacianVarianceRole, "laplacianVariance"},
        {EdgeEnergyRole, "edgeEnergy"},
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

QString ReviewModel::visualSourceFor(const QString& ticket) const {
    if (ticket.isEmpty()) {
        return {};
    }
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("generation"),
        QString::number(generation_.load(std::memory_order_acquire))
    );
    query.addQueryItem(
        QStringLiteral("ticket"),
        QString::fromLatin1(QUrl::toPercentEncoding(ticket))
    );
    return QStringLiteral("image://shadow/visual?%1")
        .arg(query.toString(QUrl::FullyEncoded));
}
