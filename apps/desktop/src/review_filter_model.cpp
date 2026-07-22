#include "review_filter_model.hpp"

#include "review_model.hpp"

#include <algorithm>

namespace {

[[nodiscard]] bool is_flag_filter(const QString& filter) {
    return filter == QStringLiteral("all")
        || filter == QStringLiteral("unflagged")
        || filter == QStringLiteral("picked")
        || filter == QStringLiteral("rejected");
}

[[nodiscard]] bool is_color_filter(const QString& filter) {
    return filter == QStringLiteral("all")
        || filter == QStringLiteral("none")
        || filter == QStringLiteral("red")
        || filter == QStringLiteral("yellow")
        || filter == QStringLiteral("green")
        || filter == QStringLiteral("blue")
        || filter == QStringLiteral("purple");
}

} // namespace

ReviewFilterModel::ReviewFilterModel(QObject* const parent)
    : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
}

QString ReviewFilterModel::flagFilter() const {
    return flag_filter_;
}

int ReviewFilterModel::minimumRating() const noexcept {
    return minimum_rating_;
}

QString ReviewFilterModel::colorFilter() const {
    return color_filter_;
}

void ReviewFilterModel::setFlagFilter(const QString& filter) {
    const QString normalized = normalizeFlagFilter(filter);
    if (flag_filter_ == normalized) {
        return;
    }
    flag_filter_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setMinimumRating(const int rating) {
    const int normalized = std::clamp(rating, 0, 5);
    if (minimum_rating_ == normalized) {
        return;
    }
    minimum_rating_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setColorFilter(const QString& filter) {
    const QString normalized = normalizeColorFilter(filter);
    if (color_filter_ == normalized) {
        return;
    }
    color_filter_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::clearFilters() {
    const bool changed = flag_filter_ != QStringLiteral("all")
        || minimum_rating_ != 0 || color_filter_ != QStringLiteral("all");
    flag_filter_ = QStringLiteral("all");
    minimum_rating_ = 0;
    color_filter_ = QStringLiteral("all");
    if (!changed) {
        return;
    }
    refreshRowsFilter();
    emit filtersChanged();
}

bool ReviewFilterModel::filterAcceptsRow(
    const int source_row,
    const QModelIndex& source_parent
) const {
    const QModelIndex row = sourceModel()->index(source_row, 0, source_parent);
    if (!row.isValid()) {
        return false;
    }
    const QString flag = sourceModel()->data(
        row,
        ReviewModel::DecisionFlagRole
    ).toString();
    if (flag_filter_ != QStringLiteral("all") && flag != flag_filter_) {
        return false;
    }
    const int rating = sourceModel()->data(
        row,
        ReviewModel::DecisionRatingRole
    ).toInt();
    if (rating < minimum_rating_) {
        return false;
    }
    const QString color = sourceModel()->data(
        row,
        ReviewModel::ColorLabelRole
    ).toString();
    return color_filter_ == QStringLiteral("all") || color == color_filter_;
}

QString ReviewFilterModel::normalizeFlagFilter(const QString& filter) {
    const QString normalized = filter.trimmed().toLower();
    return is_flag_filter(normalized) ? normalized : QStringLiteral("all");
}

QString ReviewFilterModel::normalizeColorFilter(const QString& filter) {
    const QString normalized = filter.trimmed().toLower();
    return is_color_filter(normalized) ? normalized : QStringLiteral("all");
}

void ReviewFilterModel::refreshRowsFilter() {
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}
