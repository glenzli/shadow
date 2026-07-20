#include "review_model.hpp"

#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace {

[[nodiscard]] bool is_decision_flag(const QString& flag) {
    return flag == QStringLiteral("unflagged") || flag == QStringLiteral("picked")
        || flag == QStringLiteral("rejected");
}

} // namespace

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
    case DecisionHeadSequenceRole:
        return QVariant::fromValue(item.decision_head_sequence);
    case DecisionFlagRole:
        return item.decision_flag;
    case DecisionRatingRole:
        return item.decision_rating;
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
        {DecisionHeadSequenceRole, "decisionHeadSequence"},
        {DecisionFlagRole, "decisionFlag"},
        {DecisionRatingRole, "decisionRating"},
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

std::optional<ReviewDecisionValue> ReviewModel::decisionFor(
    const QString& photo_id
) const {
    const auto item = std::find_if(
        items_.cbegin(),
        items_.cend(),
        [&photo_id](const ReviewItem& candidate) {
            return candidate.photo_id == photo_id;
        }
    );
    if (item == items_.cend()) {
        return std::nullopt;
    }
    return ReviewDecisionValue{
        .head_sequence = item->decision_head_sequence,
        .flag = item->decision_flag,
        .rating = item->decision_rating,
    };
}

bool ReviewModel::updateDecision(
    const QString& photo_id,
    const quint64 head_sequence,
    const QString& flag,
    const int rating
) {
    if (photo_id.isEmpty() || !is_decision_flag(flag) || rating < 0 || rating > 5) {
        return false;
    }
    bool found = false;
    for (qsizetype row = 0; row < items_.size(); ++row) {
        auto& item = items_[row];
        if (item.photo_id != photo_id) {
            continue;
        }
        found = true;
        if (item.decision_head_sequence == head_sequence && item.decision_flag == flag
            && item.decision_rating == rating) {
            continue;
        }
        item.decision_head_sequence = head_sequence;
        item.decision_flag = flag;
        item.decision_rating = rating;
        const QModelIndex changed = index(static_cast<int>(row), 0);
        emit dataChanged(
            changed,
            changed,
            {DecisionHeadSequenceRole, DecisionFlagRole, DecisionRatingRole}
        );
    }
    return found;
}
