#include "tone_curve_point_model.hpp"

#include <QList>
#include <QPointF>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace {

constexpr int MAXIMUM_PREVIEW_SAMPLE_COUNT = 4'097;

[[nodiscard]] bool same_nonzero_sign(
    const double left,
    const double right
) noexcept {
    return (left > 0.0 && right > 0.0) || (left < 0.0 && right < 0.0);
}

[[nodiscard]] double pchip_endpoint_derivative(
    const double first_width,
    const double second_width,
    const double first_slope,
    const double second_slope
) noexcept {
    double derivative = ((2.0 * first_width + second_width) * first_slope
                            - first_width * second_slope)
        / (first_width + second_width);
    if (first_slope == 0.0 || !same_nonzero_sign(derivative, first_slope)) {
        return 0.0;
    }
    if (!same_nonzero_sign(first_slope, second_slope)
        && std::abs(derivative) > 3.0 * std::abs(first_slope)) {
        derivative = 3.0 * first_slope;
    }
    return derivative;
}

[[nodiscard]] QVector<double> pchip_derivatives(
    const QVector<ToneCurvePoint>& points
) {
    const int point_count = static_cast<int>(points.size());
    QVector<double> widths(point_count - 1, 0.0);
    QVector<double> slopes(point_count - 1, 0.0);
    for (int index = 0; index + 1 < point_count; ++index) {
        widths[index] = points.at(index + 1).x - points.at(index).x;
        slopes[index] = (points.at(index + 1).y - points.at(index).y)
            / widths.at(index);
    }

    QVector<double> derivatives(point_count, 0.0);
    if (point_count == 2) {
        derivatives[0] = slopes[0];
        derivatives[1] = slopes[0];
        return derivatives;
    }

    derivatives[0] = pchip_endpoint_derivative(
        widths[0],
        widths[1],
        slopes[0],
        slopes[1]
    );
    for (int index = 1; index + 1 < point_count; ++index) {
        const double previous_slope = slopes.at(index - 1);
        const double next_slope = slopes.at(index);
        if (!same_nonzero_sign(previous_slope, next_slope)) {
            derivatives[index] = 0.0;
            continue;
        }
        const double previous_width = widths.at(index - 1);
        const double next_width = widths.at(index);
        const double first_weight = 2.0 * next_width + previous_width;
        const double second_weight = next_width + 2.0 * previous_width;
        derivatives[index] = (first_weight + second_weight)
            / (first_weight / previous_slope + second_weight / next_slope);
    }
    const int last_interval = static_cast<int>(widths.size()) - 1;
    derivatives[point_count - 1] = pchip_endpoint_derivative(
        widths.at(last_interval),
        widths.at(last_interval - 1),
        slopes.at(last_interval),
        slopes.at(last_interval - 1)
    );
    return derivatives;
}

[[nodiscard]] double evaluate_curve(
    const QVector<ToneCurvePoint>& points,
    const QVector<double>& derivatives,
    const double value,
    const bool smooth
) {
    const auto upper = std::upper_bound(
        points.cbegin(),
        points.cend(),
        value,
        [](const double sample, const ToneCurvePoint& point) {
            return sample < point.x;
        }
    );
    int segment = 0;
    if (upper == points.cend()) {
        segment = static_cast<int>(points.size()) - 2;
    } else if (upper != points.cbegin()) {
        segment = static_cast<int>(std::distance(points.cbegin(), upper)) - 1;
    }
    const ToneCurvePoint left = points.at(segment);
    const ToneCurvePoint right = points.at(segment + 1);
    const double width = right.x - left.x;
    if (!smooth) {
        return left.y + (value - left.x) * (right.y - left.y) / width;
    }
    if (value <= points.front().x) {
        return points.front().y
            + (value - points.front().x) * derivatives.front();
    }
    if (value >= points.back().x) {
        return points.back().y
            + (value - points.back().x) * derivatives.back();
    }
    const double t = (value - left.x) / width;
    const double t_squared = t * t;
    const double t_cubed = t_squared * t;
    const double h00 = 2.0 * t_cubed - 3.0 * t_squared + 1.0;
    const double h10 = t_cubed - 2.0 * t_squared + t;
    const double h01 = -2.0 * t_cubed + 3.0 * t_squared;
    const double h11 = t_cubed - t_squared;
    return h00 * left.y + h10 * width * derivatives.at(segment)
        + h01 * right.y + h11 * width * derivatives.at(segment + 1);
}

} // namespace

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

QVariantList ToneCurvePointModel::sampledPoints(
    const int sample_count,
    const bool smooth
) const {
    if (sample_count < 2 || sample_count > MAXIMUM_PREVIEW_SAMPLE_COUNT
        || !isValidPersistedCurve(points_)) {
        return {};
    }
    const QVector<double> derivatives = smooth
        ? pchip_derivatives(points_)
        : QVector<double>{};
    const bool identity = points_.size() == 2
        && points_.at(0) == ToneCurvePoint{0.0, 0.0}
        && points_.at(1) == ToneCurvePoint{1.0, 1.0};
    QVariantList samples;
    samples.reserve(sample_count);
    const double denominator = static_cast<double>(sample_count - 1);
    for (int index = 0; index < sample_count; ++index) {
        const double x = static_cast<double>(index) / denominator;
        const double y = smooth && identity
            ? x
            : evaluate_curve(points_, derivatives, x, smooth);
        if (!std::isfinite(y)) {
            return {};
        }
        samples.push_back(QVariant::fromValue(QPointF{x, y}));
    }
    return samples;
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
