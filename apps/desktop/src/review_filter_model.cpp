#include "review_filter_model.hpp"

#include "review_model.hpp"

#include <QRegularExpression>

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

[[nodiscard]] bool is_edit_filter(const QString& filter) {
    return filter == QStringLiteral("all")
        || filter == QStringLiteral("edited")
        || filter == QStringLiteral("unedited");
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

QString ReviewFilterModel::editFilter() const {
    return edit_filter_;
}

QString ReviewFilterModel::captureMonth() const {
    return capture_month_;
}

QString ReviewFilterModel::cameraKey() const {
    return camera_key_;
}

QString ReviewFilterModel::lensKey() const {
    return lens_key_;
}

bool ReviewFilterModel::hasActiveServerFilter() const {
    return flag_filter_ != QStringLiteral("all") || minimum_rating_ > 0
        || color_filter_ != QStringLiteral("all")
        || edit_filter_ != QStringLiteral("all") || !capture_month_.isEmpty()
        || !camera_key_.isEmpty() || !lens_key_.isEmpty();
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

void ReviewFilterModel::setEditFilter(const QString& filter) {
    const QString normalized = normalizeEditFilter(filter);
    if (edit_filter_ == normalized) {
        return;
    }
    edit_filter_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setCaptureMonth(const QString& capture_month) {
    const QString normalized = normalizeCaptureMonth(capture_month);
    if (capture_month_ == normalized) {
        return;
    }
    capture_month_ = normalized;
    emit filtersChanged();
}

void ReviewFilterModel::setCameraKey(const QString& camera_key) {
    const QString normalized = normalizeFacetKey(camera_key);
    if (camera_key_ == normalized) {
        return;
    }
    camera_key_ = normalized;
    emit filtersChanged();
}

void ReviewFilterModel::setLensKey(const QString& lens_key) {
    const QString normalized = normalizeFacetKey(lens_key);
    if (lens_key_ == normalized) {
        return;
    }
    lens_key_ = normalized;
    emit filtersChanged();
}

void ReviewFilterModel::clearFilters() {
    const bool changed = flag_filter_ != QStringLiteral("all")
        || minimum_rating_ != 0 || color_filter_ != QStringLiteral("all")
        || edit_filter_ != QStringLiteral("all") || !capture_month_.isEmpty()
        || !camera_key_.isEmpty() || !lens_key_.isEmpty();
    flag_filter_ = QStringLiteral("all");
    minimum_rating_ = 0;
    color_filter_ = QStringLiteral("all");
    edit_filter_ = QStringLiteral("all");
    capture_month_.clear();
    camera_key_.clear();
    lens_key_.clear();
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
    if (color_filter_ != QStringLiteral("all") && color != color_filter_) {
        return false;
    }
    const bool edited = sourceModel()->data(
        row,
        ReviewModel::HasDevelopmentEditsRole
    ).toBool();
    return edit_filter_ == QStringLiteral("all")
        || (edit_filter_ == QStringLiteral("edited") && edited)
        || (edit_filter_ == QStringLiteral("unedited") && !edited);
}

QString ReviewFilterModel::normalizeFlagFilter(const QString& filter) {
    const QString normalized = filter.trimmed().toLower();
    return is_flag_filter(normalized) ? normalized : QStringLiteral("all");
}

QString ReviewFilterModel::normalizeColorFilter(const QString& filter) {
    const QString normalized = filter.trimmed().toLower();
    return is_color_filter(normalized) ? normalized : QStringLiteral("all");
}

QString ReviewFilterModel::normalizeEditFilter(const QString& filter) {
    const QString normalized = filter.trimmed().toLower();
    return is_edit_filter(normalized) ? normalized : QStringLiteral("all");
}

QString ReviewFilterModel::normalizeCaptureMonth(const QString& value) {
    const QString normalized = value.trimmed();
    if (normalized.isEmpty()) {
        return {};
    }
    const bool valid = normalized.size() == 7 && normalized.at(4) == u'-'
        && normalized.left(4).toInt() >= 0
        && normalized.mid(5, 2).toInt() >= 1
        && normalized.mid(5, 2).toInt() <= 12
        && normalized.left(4).contains(QRegularExpression(QStringLiteral("^[0-9]{4}$")))
        && normalized.mid(5, 2).contains(QRegularExpression(QStringLiteral("^[0-9]{2}$")));
    return valid ? normalized : QString{};
}

QString ReviewFilterModel::normalizeFacetKey(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    return normalized.size() <= 512 ? normalized : QString{};
}

void ReviewFilterModel::refreshRowsFilter() {
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}
