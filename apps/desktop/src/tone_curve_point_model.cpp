#include "tone_curve_point_model.hpp"

#include <QList>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

ToneCurvePointModel::ToneCurvePointModel(QObject* parent)
    : QAbstractListModel(parent),
      points_{{0.0, 0.0}, {1.0, 1.0}} {}

int ToneCurvePointModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : pointCount();
}

QVariant ToneCurvePointModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= pointCount()) {
        return {};
    }
    const int row = index.row();
    const auto& point = points_.at(row);
    switch (role) {
    case XRole:
        return point.x;
    case YRole:
        return point.y;
    case EndpointRole:
        return isEndpoint(row);
    case XMovableRole:
        return editable_ && !isEndpoint(row);
    case DeletableRole:
        return editable_ && !isEndpoint(row) && pointCount() > minimum_stored_point_count;
    case SelectedRole:
        return row == selected_index_;
    default:
        return {};
    }
}

QHash<int, QByteArray> ToneCurvePointModel::roleNames() const {
    return {
        {XRole, "xValue"},
        {YRole, "yValue"},
        {EndpointRole, "endpoint"},
        {XMovableRole, "xMovable"},
        {DeletableRole, "deletable"},
        {SelectedRole, "selected"},
    };
}

QVector<ToneCurvePoint> ToneCurvePointModel::points() const {
    return points_;
}

int ToneCurvePointModel::pointCount() const noexcept {
    return static_cast<int>(points_.size());
}

bool ToneCurvePointModel::isEditable() const noexcept {
    return editable_;
}

int ToneCurvePointModel::selectedIndex() const noexcept {
    return selected_index_;
}

bool ToneCurvePointModel::replace(QVector<ToneCurvePoint> points) {
    if (!isValidPersistedCurve(points)) {
        return false;
    }

    const int old_count = pointCount();
    const int old_selection = selected_index_;
    const bool old_editable = editable_;
    const bool new_editable = supportsActiveEditing(points);

    beginResetModel();
    points_ = std::move(points);
    selected_index_ = -1;
    editable_ = new_editable;
    endResetModel();

    if (old_count != pointCount()) {
        emit pointCountChanged();
    }
    if (old_selection != -1) {
        emit selectedIndexChanged();
    }
    if (old_editable != editable_) {
        emit editableChanged();
    }
    emit pointsChanged();
    return true;
}

bool ToneCurvePointModel::selectPoint(const int row) {
    if (row < -1 || row >= pointCount()) {
        return false;
    }
    if (row == selected_index_) {
        return true;
    }

    const int old_selection = selected_index_;
    selected_index_ = row;
    emitSelectionDataChanged(old_selection, selected_index_);
    emit selectedIndexChanged();
    return true;
}

bool ToneCurvePointModel::movePoint(const int row, const double x, const double y) {
    if (!editable_ || row < 0 || row >= pointCount() || !std::isfinite(x)
        || !std::isfinite(y)) {
        return false;
    }

    auto next = points_.at(row);
    if (!isEndpoint(row)) {
        const double lower = points_.at(row - 1).x + minimum_x_spacing;
        const double upper = points_.at(row + 1).x - minimum_x_spacing;
        if (lower > upper) {
            return false;
        }
        next.x = std::clamp(x, lower, upper);
    }
    next.y = std::clamp(y, 0.0, 1.0);

    QList<int> changed_roles;
    if (next.x != points_.at(row).x) {
        changed_roles.append(XRole);
    }
    if (next.y != points_.at(row).y) {
        changed_roles.append(YRole);
    }
    if (changed_roles.isEmpty()) {
        return false;
    }

    points_[row] = next;
    emit dataChanged(index(row, 0), index(row, 0), changed_roles);
    emit pointsChanged();
    return true;
}

int ToneCurvePointModel::addPoint(const double x, const double y) {
    if (!editable_ || pointCount() >= maximum_active_point_count || !std::isfinite(x)
        || !std::isfinite(y)) {
        return -1;
    }

    const double bounded_x = std::clamp(x, minimum_x_spacing, 1.0 - minimum_x_spacing);
    const auto position = std::upper_bound(
        points_.cbegin(),
        points_.cend(),
        bounded_x,
        [](const double value, const ToneCurvePoint& point) { return value < point.x; }
    );
    const int row = static_cast<int>(std::distance(points_.cbegin(), position));
    if (row <= 0 || row >= pointCount()) {
        return -1;
    }

    const double lower = points_.at(row - 1).x + minimum_x_spacing;
    const double upper = points_.at(row).x - minimum_x_spacing;
    if (lower > upper) {
        return -1;
    }
    const ToneCurvePoint point{
        std::clamp(bounded_x, lower, upper),
        std::clamp(y, 0.0, 1.0),
    };

    const int old_selection_after_insert = selected_index_ >= row
        ? selected_index_ + 1
        : selected_index_;
    beginInsertRows({}, row, row);
    points_.insert(row, point);
    endInsertRows();

    selected_index_ = row;
    emitSelectionDataChanged(old_selection_after_insert, selected_index_);
    emit pointCountChanged();
    emit selectedIndexChanged();
    emit pointsChanged();
    return row;
}

bool ToneCurvePointModel::removePoint(const int row) {
    if (!editable_ || row <= 0 || row >= pointCount() - 1
        || pointCount() <= minimum_stored_point_count) {
        return false;
    }

    const int old_selection = selected_index_;
    beginRemoveRows({}, row, row);
    points_.removeAt(row);
    endRemoveRows();

    const bool removed_selection = old_selection == row;
    if (removed_selection) {
        selected_index_ = std::min(row, pointCount() - 1);
        emit dataChanged(
            index(selected_index_, 0),
            index(selected_index_, 0),
            {SelectedRole}
        );
    } else if (old_selection > row) {
        selected_index_ = old_selection - 1;
    }

    emit pointCountChanged();
    if (removed_selection || selected_index_ != old_selection) {
        emit selectedIndexChanged();
    }
    emit pointsChanged();
    return true;
}

void ToneCurvePointModel::resetLinear() {
    const bool replaced = replace({{0.0, 0.0}, {1.0, 1.0}});
    Q_ASSERT(replaced);
    static_cast<void>(replaced);
}

bool ToneCurvePointModel::isValidPersistedCurve(
    const QVector<ToneCurvePoint>& points
) noexcept {
    if (points.size() < minimum_stored_point_count
        || points.size() > maximum_stored_point_count) {
        return false;
    }
    if (points.front().x != 0.0 || points.back().x != 1.0) {
        return false;
    }
    for (int row = 0; row < static_cast<int>(points.size()); ++row) {
        const auto& point = points.at(row);
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            return false;
        }
        if (row > 0 && point.x <= points.at(row - 1).x) {
            return false;
        }
    }
    return true;
}

bool ToneCurvePointModel::supportsActiveEditing(
    const QVector<ToneCurvePoint>& points
) noexcept {
    if (points.size() > maximum_active_point_count) {
        return false;
    }
    for (int row = 0; row < static_cast<int>(points.size()); ++row) {
        const auto& point = points.at(row);
        if (point.y < 0.0 || point.y > 1.0) {
            return false;
        }
        if (row > 0 && point.x - points.at(row - 1).x < minimum_x_spacing) {
            return false;
        }
    }
    return true;
}

bool ToneCurvePointModel::isEndpoint(const int row) const noexcept {
    return row == 0 || row == pointCount() - 1;
}

void ToneCurvePointModel::emitSelectionDataChanged(const int old_row, const int new_row) {
    if (old_row < 0 && new_row < 0) {
        return;
    }
    const int first = old_row < 0 ? new_row : (new_row < 0 ? old_row : std::min(old_row, new_row));
    const int last = old_row < 0 ? new_row : (new_row < 0 ? old_row : std::max(old_row, new_row));
    emit dataChanged(index(first, 0), index(last, 0), {SelectedRole});
}
