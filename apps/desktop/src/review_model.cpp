#include "review_model.hpp"

#include <QSet>
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

[[nodiscard]] bool has_unique_stable_keys(const QVector<ReviewItem>& items) {
    QSet<QString> keys;
    keys.reserve(items.size());
    for (const auto& item : items) {
        if (item.representation_id.isEmpty()
            || keys.contains(item.representation_id)) {
            return false;
        }
        keys.insert(item.representation_id);
    }
    return true;
}

void append_role(QList<int>& roles, const int role) {
    if (!roles.contains(role)) {
        roles.append(role);
    }
}

[[nodiscard]] QList<int> changed_roles(
    const ReviewItem& current,
    const ReviewItem& replacement
) {
    QList<int> roles;
    if (current.photo_id != replacement.photo_id) {
        append_role(roles, ReviewModel::PhotoIdRole);
    }
    if (current.visual_handle != replacement.visual_handle) {
        append_role(roles, ReviewModel::VisualHandleRole);
        append_role(roles, ReviewModel::VisualSourceRole);
    }
    if (current.title != replacement.title) {
        append_role(roles, ReviewModel::TitleRole);
    }
    if (current.source_path != replacement.source_path) {
        append_role(roles, ReviewModel::SourcePathRole);
    }
    if (current.visual_role != replacement.visual_role) {
        append_role(roles, ReviewModel::VisualRole);
    }
    if (current.has_visual != replacement.has_visual) {
        append_role(roles, ReviewModel::VisualErrorRole);
        append_role(roles, ReviewModel::VisualSourceRole);
    }
    if (current.visual_width != replacement.visual_width) {
        append_role(roles, ReviewModel::VisualWidthRole);
    }
    if (current.visual_height != replacement.visual_height) {
        append_role(roles, ReviewModel::VisualHeightRole);
    }
    if (current.has_technical_observation
        != replacement.has_technical_observation) {
        append_role(roles, ReviewModel::HasTechnicalObservationRole);
    }
    if (current.technical_input_width != replacement.technical_input_width) {
        append_role(roles, ReviewModel::TechnicalInputWidthRole);
    }
    if (current.technical_input_height != replacement.technical_input_height) {
        append_role(roles, ReviewModel::TechnicalInputHeightRole);
    }
    if (current.technical_preprocessing_version
        != replacement.technical_preprocessing_version) {
        append_role(roles, ReviewModel::TechnicalPreprocessingVersionRole);
    }
    if (current.technical_implementation_version
        != replacement.technical_implementation_version) {
        append_role(roles, ReviewModel::TechnicalImplementationVersionRole);
    }
    if (current.mean_luma != replacement.mean_luma) {
        append_role(roles, ReviewModel::MeanLumaRole);
    }
    if (current.p01_luma != replacement.p01_luma) {
        append_role(roles, ReviewModel::P01LumaRole);
    }
    if (current.p50_luma != replacement.p50_luma) {
        append_role(roles, ReviewModel::P50LumaRole);
    }
    if (current.p99_luma != replacement.p99_luma) {
        append_role(roles, ReviewModel::P99LumaRole);
    }
    if (current.near_black_fraction != replacement.near_black_fraction) {
        append_role(roles, ReviewModel::NearBlackFractionRole);
    }
    if (current.near_white_fraction != replacement.near_white_fraction) {
        append_role(roles, ReviewModel::NearWhiteFractionRole);
    }
    if (current.laplacian_variance != replacement.laplacian_variance) {
        append_role(roles, ReviewModel::LaplacianVarianceRole);
    }
    if (current.edge_energy != replacement.edge_energy) {
        append_role(roles, ReviewModel::EdgeEnergyRole);
    }
    if (current.decision_head_sequence != replacement.decision_head_sequence) {
        append_role(roles, ReviewModel::DecisionHeadSequenceRole);
    }
    if (current.decision_flag != replacement.decision_flag) {
        append_role(roles, ReviewModel::DecisionFlagRole);
    }
    if (current.decision_rating != replacement.decision_rating) {
        append_role(roles, ReviewModel::DecisionRatingRole);
    }
    return roles;
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

bool ReviewModel::appendSnapshot(
    QVector<ReviewItem> items,
    const quint64 generation
) {
    if (!isGenerationCurrent(generation) || !has_unique_stable_keys(items)
        || !has_unique_stable_keys(items_)) {
        return false;
    }

    QSet<QString> existing_keys;
    existing_keys.reserve(items_.size());
    for (const auto& item : items_) {
        existing_keys.insert(item.representation_id);
    }
    for (const auto& item : items) {
        if (existing_keys.contains(item.representation_id)) {
            return false;
        }
    }
    append(std::move(items));
    return true;
}

bool ReviewModel::reconcileSnapshot(
    QVector<ReviewItem> items,
    const quint64 generation
) {
    if (!isGenerationCurrent(generation) || !has_unique_stable_keys(items)
        || !has_unique_stable_keys(items_)) {
        return false;
    }

    QSet<QString> desired_keys;
    desired_keys.reserve(items.size());
    for (const auto& item : items) {
        desired_keys.insert(item.representation_id);
    }

    qsizetype row = items_.size();
    while (row > 0) {
        --row;
        if (desired_keys.contains(items_.at(row).representation_id)) {
            continue;
        }

        const qsizetype last = row;
        while (row > 0
               && !desired_keys.contains(items_.at(row - 1).representation_id)) {
            --row;
        }
        const qsizetype first = row;
        beginRemoveRows({}, static_cast<int>(first), static_cast<int>(last));
        items_.remove(first, last - first + 1);
        endRemoveRows();
    }

    for (qsizetype target_row = 0; target_row < items.size(); ++target_row) {
        const auto& desired = items.at(target_row);
        if (target_row >= items_.size()
            || items_.at(target_row).representation_id
                != desired.representation_id) {
            const auto existing = std::find_if(
                items_.cbegin() + std::min(target_row, items_.size()),
                items_.cend(),
                [&desired](const ReviewItem& candidate) {
                    return candidate.representation_id == desired.representation_id;
                }
            );
            if (existing == items_.cend()) {
                beginInsertRows(
                    {},
                    static_cast<int>(target_row),
                    static_cast<int>(target_row)
                );
                items_.insert(target_row, desired);
                endInsertRows();
                continue;
            }

            const qsizetype source_row = existing - items_.cbegin();
            beginMoveRows(
                {},
                static_cast<int>(source_row),
                static_cast<int>(source_row),
                {},
                static_cast<int>(target_row)
            );
            items_.move(source_row, target_row);
            endMoveRows();
        }

        const QList<int> roles = changed_roles(items_.at(target_row), desired);
        if (roles.isEmpty()) {
            continue;
        }
        items_[target_row] = desired;
        const QModelIndex changed = index(static_cast<int>(target_row), 0);
        emit dataChanged(changed, changed, roles);
    }
    return true;
}

bool ReviewModel::reconcilePrefixSnapshot(
    QVector<ReviewItem> items,
    const quint64 generation
) {
    if (!isGenerationCurrent(generation) || !has_unique_stable_keys(items)
        || !has_unique_stable_keys(items_)) {
        return false;
    }

    QSet<QString> prefix_keys;
    prefix_keys.reserve(items.size());
    for (const auto& item : items) {
        prefix_keys.insert(item.representation_id);
    }

    items.reserve(items.size() + items_.size());
    for (const auto& existing : items_) {
        if (!prefix_keys.contains(existing.representation_id)) {
            items.push_back(existing);
        }
    }
    return reconcileSnapshot(std::move(items), generation);
}

QVector<QString> ReviewModel::representationIds() const {
    QVector<QString> ids;
    ids.reserve(items_.size());
    for (const auto& item : items_) {
        ids.push_back(item.representation_id);
    }
    return ids;
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
