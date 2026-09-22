#include "review_filter_model.hpp"

#include "review_model.hpp"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

int ReviewFilterModel::indexOfPhoto(
    const QString& photo_id,
    const QString& representation_id
) const {
    if (photo_id.isEmpty() || representation_id.isEmpty())
        return -1;
    for (int row = 0; row < rowCount(); ++row) {
        const QModelIndex candidate = index(row, 0);
        if (data(candidate, ReviewModel::PhotoIdRole).toString() == photo_id
            && data(candidate, ReviewModel::RepresentationIdRole).toString() == representation_id)
            return row;
    }
    return -1;
}

namespace {

// Match Catalog's library_equipment_key: trim, ASCII lowercase, omit empty parts.
[[nodiscard]] QString equipment_key(const QString& make, const QString& model) {
    QStringList parts;
    for (QString part : {make.trimmed(), model.trimmed()}) {
        if (part.isEmpty())
            continue;
        for (qsizetype i = 0; i < part.size(); ++i) {
            const ushort c = part.at(i).unicode();
            if (c >= 'A' && c <= 'Z')
                part[i] = QChar(static_cast<ushort>(c + ('a' - 'A')));
        }
        parts.append(part);
    }
    return parts.join(QChar(0x001f));
}

[[nodiscard]] bool is_flag_filter(const QString& filter) {
    return filter == QStringLiteral("all") || filter == QStringLiteral("unflagged")
           || filter == QStringLiteral("picked") || filter == QStringLiteral("rejected");
}

[[nodiscard]] bool is_color_filter(const QString& filter) {
    return filter == QStringLiteral("all") || filter == QStringLiteral("none")
           || filter == QStringLiteral("red") || filter == QStringLiteral("yellow")
           || filter == QStringLiteral("green") || filter == QStringLiteral("blue")
           || filter == QStringLiteral("purple");
}

[[nodiscard]] bool is_edit_filter(const QString& filter) {
    return filter == QStringLiteral("all") || filter == QStringLiteral("edited")
           || filter == QStringLiteral("unedited");
}

[[nodiscard]] bool is_liked_filter(const QString& filter) {
    return filter == QStringLiteral("all") || filter == QStringLiteral("liked")
           || filter == QStringLiteral("unliked");
}

[[nodiscard]] bool is_chinese_lunar_month_type(const QString& value) {
    return value == QStringLiteral("all") || value == QStringLiteral("regular")
           || value == QStringLiteral("leap");
}

[[nodiscard]] QString semantic_key(const QAbstractItemModel& model, const QModelIndex& row) {
    return model.data(row, ReviewModel::PhotoIdRole).toString() + QChar{0x001f}
           + model.data(row, ReviewModel::RepresentationIdRole).toString();
}

} // namespace

ReviewFilterModel::ReviewFilterModel(QObject* const parent) : QSortFilterProxyModel(parent) {
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

bool ReviewFilterModel::hideOfflineUncached() const noexcept {
    return hide_offline_uncached_;
}
bool ReviewFilterModel::onlyEditable() const noexcept {
    return only_editable_;
}

void ReviewFilterModel::setHideOfflineUncached(const bool enabled) {
    if (hide_offline_uncached_ == enabled)
        return;
    hide_offline_uncached_ = enabled;
    refreshRowsFilter();
    emit availabilityFiltersChanged();
}

void ReviewFilterModel::setOnlyEditable(const bool enabled) {
    if (only_editable_ == enabled)
        return;
    only_editable_ = enabled;
    refreshRowsFilter();
    emit availabilityFiltersChanged();
}

QString ReviewFilterModel::likedFilter() const {
    return liked_filter_;
}

QString ReviewFilterModel::excludedFlagFilter() const {
    return excluded_flag_filter_;
}

QString ReviewFilterModel::excludedColorFilter() const {
    return excluded_color_filter_;
}

QString ReviewFilterModel::captureMonth() const {
    return capture_month_;
}

int ReviewFilterModel::chineseLunarMonth() const noexcept {
    return chinese_lunar_month_;
}

int ReviewFilterModel::chineseLunarDay() const noexcept {
    return chinese_lunar_day_;
}

QString ReviewFilterModel::chineseLunarMonthType() const {
    return chinese_lunar_month_type_;
}

QString ReviewFilterModel::cameraKey() const {
    return camera_key_;
}

QString ReviewFilterModel::lensKey() const {
    return lens_key_;
}

QString ReviewFilterModel::countryKey() const {
    return country_key_;
}

QString ReviewFilterModel::localityKey() const {
    return locality_key_;
}

bool ReviewFilterModel::travelFilterEnabled() const noexcept {
    return travel_filter_enabled_;
}

bool ReviewFilterModel::dailyFilterEnabled() const noexcept {
    return daily_filter_enabled_;
}

QStringList ReviewFilterModel::keywordIdsAll() const {
    return keyword_ids_all_;
}

QStringList ReviewFilterModel::excludedKeywordIdsAny() const {
    return excluded_keyword_ids_any_;
}

bool ReviewFilterModel::hasActiveServerFilter() const {
    return flag_filter_ != QStringLiteral("all") || minimum_rating_ > 0
           || color_filter_ != QStringLiteral("all") || edit_filter_ != QStringLiteral("all")
           || liked_filter_ != QStringLiteral("all") || !capture_month_.isEmpty()
           || chinese_lunar_month_ > 0 || chinese_lunar_day_ > 0
           || chinese_lunar_month_type_ != QStringLiteral("all") || !camera_key_.isEmpty()
           || !lens_key_.isEmpty() || !country_key_.isEmpty() || !locality_key_.isEmpty()
           || travel_filter_enabled_ || daily_filter_enabled_ || !keyword_ids_all_.isEmpty()
           || !excluded_keyword_ids_any_.isEmpty();
}

bool ReviewFilterModel::semanticFilterActive() const noexcept {
    return !semantic_rank_by_key_.isEmpty();
}

QStringList ReviewFilterModel::semanticRepresentationKeys() const {
    return semantic_rank_by_key_.keys();
}

QStringList ReviewFilterModel::smartCategoryRepresentationKeys() const {
    return smart_category_keys_.values();
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

void ReviewFilterModel::setLikedFilter(const QString& filter) {
    const QString normalized = normalizeLikedFilter(filter);
    if (liked_filter_ == normalized) {
        return;
    }
    liked_filter_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setExcludedFlagFilter(const QString& filter) {
    const QString normalized = normalizeFlagFilter(filter);
    if (excluded_flag_filter_ == normalized) {
        return;
    }
    excluded_flag_filter_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setExcludedColorFilter(const QString& filter) {
    const QString normalized = normalizeColorFilter(filter);
    if (excluded_color_filter_ == normalized) {
        return;
    }
    excluded_color_filter_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setCaptureMonth(const QString& capture_month) {
    const QString normalized = normalizeCaptureMonth(capture_month);
    if (capture_month_ == normalized) {
        return;
    }
    capture_month_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setChineseLunarMonth(const int month) {
    const int normalized = (month >= 1 && month <= 12) ? month : 0;
    if (chinese_lunar_month_ == normalized) {
        return;
    }
    chinese_lunar_month_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setChineseLunarDay(const int day) {
    const int normalized = (day >= 1 && day <= 30) ? day : 0;
    if (chinese_lunar_day_ == normalized) {
        return;
    }
    chinese_lunar_day_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setChineseLunarMonthType(const QString& month_type) {
    const QString normalized = normalizeChineseLunarMonthType(month_type);
    if (chinese_lunar_month_type_ == normalized) {
        return;
    }
    chinese_lunar_month_type_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setCameraKey(const QString& camera_key) {
    const QString normalized = normalizeFacetKey(camera_key);
    if (camera_key_ == normalized) {
        return;
    }
    camera_key_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setLensKey(const QString& lens_key) {
    const QString normalized = normalizeFacetKey(lens_key);
    if (lens_key_ == normalized) {
        return;
    }
    lens_key_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setCountryKey(const QString& country_key) {
    const QString normalized = normalizeFacetKey(country_key);
    if (country_key_ == normalized) {
        return;
    }
    country_key_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setLocalityKey(const QString& locality_key) {
    const QString normalized = normalizeFacetKey(locality_key);
    if (locality_key_ == normalized) {
        return;
    }
    locality_key_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setTravelFilterEnabled(const bool enabled) {
    if (travel_filter_enabled_ == enabled && (!enabled || !daily_filter_enabled_)) {
        return;
    }
    travel_filter_enabled_ = enabled;
    if (enabled) {
        daily_filter_enabled_ = false;
    }
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setDailyFilterEnabled(const bool enabled) {
    if (daily_filter_enabled_ == enabled && (!enabled || !travel_filter_enabled_)) {
        return;
    }
    daily_filter_enabled_ = enabled;
    if (enabled) {
        travel_filter_enabled_ = false;
    }
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setKeywordIdsAll(const QStringList& keyword_ids) {
    const QStringList normalized = normalizeKeywordIds(keyword_ids);
    if (keyword_ids_all_ == normalized) {
        return;
    }
    keyword_ids_all_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setExcludedKeywordIdsAny(const QStringList& keyword_ids) {
    const QStringList normalized = normalizeKeywordIds(keyword_ids);
    if (excluded_keyword_ids_any_ == normalized) {
        return;
    }
    excluded_keyword_ids_any_ = normalized;
    refreshRowsFilter();
    emit filtersChanged();
}

void ReviewFilterModel::setSemanticRepresentationOrder(const QStringList& ranked_keys) {
    QHash<QString, qsizetype> next;
    next.reserve(ranked_keys.size());
    for (const QString& key : ranked_keys) {
        if (!key.isEmpty() && !next.contains(key)) {
            next.insert(key, next.size());
        }
    }
    if (semantic_rank_by_key_ == next) {
        return;
    }
    semantic_rank_by_key_ = std::move(next);
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    sort(semantic_rank_by_key_.isEmpty() ? -1 : 0);
    emit semanticFilterChanged();
}

bool ReviewFilterModel::smartCategoryFilterActive() const noexcept {
    return !smart_category_keys_.isEmpty();
}

void ReviewFilterModel::setSmartCategoryRepresentationKeys(const QStringList& member_keys) {
    QSet<QString> next;
    next.reserve(member_keys.size());
    for (const QString& key : member_keys)
        if (!key.isEmpty())
            next.insert(key);
    if (smart_category_keys_ == next)
        return;
    smart_category_keys_ = std::move(next);
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    emit smartCategoryFilterChanged();
}

void ReviewFilterModel::setPhotoScope(const bool enabled, const QStringList& photo_ids) {
    const QSet<QString> next =
        enabled ? QSet<QString>(photo_ids.cbegin(), photo_ids.cend()) : QSet<QString>{};
    if (photo_scope_enabled_ == enabled && photo_scope_ids_ == next) {
        return;
    }
    photo_scope_enabled_ = enabled;
    photo_scope_ids_ = next;
    refreshRowsFilter();
}

void ReviewFilterModel::clearFilters() {
    const bool changed =
        hide_offline_uncached_ || only_editable_ || flag_filter_ != QStringLiteral("all")
        || minimum_rating_ != 0 || color_filter_ != QStringLiteral("all")
        || edit_filter_ != QStringLiteral("all") || liked_filter_ != QStringLiteral("all")
        || excluded_flag_filter_ != QStringLiteral("all")
        || excluded_color_filter_ != QStringLiteral("all") || !capture_month_.isEmpty()
        || chinese_lunar_month_ > 0 || chinese_lunar_day_ > 0
        || chinese_lunar_month_type_ != QStringLiteral("all") || !camera_key_.isEmpty()
        || !lens_key_.isEmpty() || !country_key_.isEmpty() || !locality_key_.isEmpty()
        || travel_filter_enabled_ || daily_filter_enabled_ || !keyword_ids_all_.isEmpty()
        || !excluded_keyword_ids_any_.isEmpty() || !semantic_rank_by_key_.isEmpty()
        || !smart_category_keys_.isEmpty();
    const bool availability_changed = hide_offline_uncached_ || only_editable_;
    const bool server_filters_changed =
        flag_filter_ != QStringLiteral("all") || minimum_rating_ != 0
        || color_filter_ != QStringLiteral("all") || edit_filter_ != QStringLiteral("all")
        || liked_filter_ != QStringLiteral("all") || excluded_flag_filter_ != QStringLiteral("all")
        || excluded_color_filter_ != QStringLiteral("all") || !capture_month_.isEmpty()
        || chinese_lunar_month_ > 0 || chinese_lunar_day_ > 0
        || chinese_lunar_month_type_ != QStringLiteral("all") || !camera_key_.isEmpty()
        || !lens_key_.isEmpty() || !country_key_.isEmpty() || !locality_key_.isEmpty()
        || travel_filter_enabled_ || daily_filter_enabled_ || !keyword_ids_all_.isEmpty()
        || !excluded_keyword_ids_any_.isEmpty();
    const bool semantic_filter_changed = !semantic_rank_by_key_.isEmpty();
    const bool smart_category_filter_changed = !smart_category_keys_.isEmpty();
    hide_offline_uncached_ = false;
    only_editable_ = false;
    flag_filter_ = QStringLiteral("all");
    minimum_rating_ = 0;
    color_filter_ = QStringLiteral("all");
    edit_filter_ = QStringLiteral("all");
    liked_filter_ = QStringLiteral("all");
    excluded_flag_filter_ = QStringLiteral("all");
    excluded_color_filter_ = QStringLiteral("all");
    capture_month_.clear();
    chinese_lunar_month_ = 0;
    chinese_lunar_day_ = 0;
    chinese_lunar_month_type_ = QStringLiteral("all");
    camera_key_.clear();
    lens_key_.clear();
    country_key_.clear();
    locality_key_.clear();
    travel_filter_enabled_ = false;
    daily_filter_enabled_ = false;
    keyword_ids_all_.clear();
    excluded_keyword_ids_any_.clear();
    semantic_rank_by_key_.clear();
    smart_category_keys_.clear();
    if (!changed) {
        return;
    }
    refreshRowsFilter();
    sort(-1);
    if (availability_changed)
        emit availabilityFiltersChanged();
    if (server_filters_changed) {
        emit filtersChanged();
    }
    if (semantic_filter_changed) {
        emit semanticFilterChanged();
    }
    if (smart_category_filter_changed)
        emit smartCategoryFilterChanged();
}

bool ReviewFilterModel::filterAcceptsRow(
    const int source_row,
    const QModelIndex& source_parent
) const {
    const QModelIndex row = sourceModel()->index(source_row, 0, source_parent);
    if (!row.isValid()) {
        return false;
    }
    if (photo_scope_enabled_
        && !photo_scope_ids_.contains(
            sourceModel()->data(row, ReviewModel::PhotoIdRole).toString()
        )) {
        return false;
    }
    if (!semantic_rank_by_key_.isEmpty()
        && !semantic_rank_by_key_.contains(semantic_key(*sourceModel(), row))) {
        return false;
    }
    if (!smart_category_keys_.isEmpty()
        && !smart_category_keys_.contains(semantic_key(*sourceModel(), row))) {
        return false;
    }
    const bool remote = sourceModel()->data(row, ReviewModel::IsRemoteRole).toBool();
    if (only_editable_ && !sourceModel()->data(row, ReviewModel::SourceAvailableRole).toBool()) {
        return false;
    }
    if (hide_offline_uncached_ && remote
        && sourceModel()->data(row, ReviewModel::RemoteOfflineRole).toBool()
        && !sourceModel()->data(row, ReviewModel::RemoteOriginalCachedRole).toBool()
        && sourceModel()->data(row, ReviewModel::VisualSourceRole).toString().isEmpty()) {
        return false;
    }
    if (remote
        && (chinese_lunar_month_ > 0 || chinese_lunar_day_ > 0
            || chinese_lunar_month_type_ != QStringLiteral("all") || !country_key_.isEmpty()
            || !locality_key_.isEmpty() || travel_filter_enabled_ || daily_filter_enabled_
            || !keyword_ids_all_.isEmpty() || !excluded_keyword_ids_any_.isEmpty())) {
        return false;
    }
    if (remote) {
        const auto value = [&](const int role) {
            return sourceModel()->data(row, role).toString();
        };
        if (!capture_month_.isEmpty()
            && value(ReviewModel::CaptureDayRole).left(7) != capture_month_)
            return false;
        if (!camera_key_.isEmpty()
            && equipment_key(
                   value(ReviewModel::CameraMakeRole),
                   value(ReviewModel::CameraModelRole)
               ) != camera_key_)
            return false;
        if (!lens_key_.isEmpty()
            && equipment_key(value(ReviewModel::LensMakeRole), value(ReviewModel::LensModelRole))
                   != lens_key_)
            return false;
    }
    const QString flag = sourceModel()->data(row, ReviewModel::DecisionFlagRole).toString();
    if (flag_filter_ != QStringLiteral("all") && flag != flag_filter_) {
        return false;
    }
    if (excluded_flag_filter_ != QStringLiteral("all") && flag == excluded_flag_filter_) {
        return false;
    }
    const int rating = sourceModel()->data(row, ReviewModel::DecisionRatingRole).toInt();
    if (rating < minimum_rating_) {
        return false;
    }
    const QString color = sourceModel()->data(row, ReviewModel::ColorLabelRole).toString();
    if (color_filter_ != QStringLiteral("all") && color != color_filter_) {
        return false;
    }
    if (excluded_color_filter_ != QStringLiteral("all") && color == excluded_color_filter_) {
        return false;
    }
    const bool edited = sourceModel()->data(row, ReviewModel::HasDevelopmentEditsRole).toBool();
    if (!(edit_filter_ == QStringLiteral("all")
          || (edit_filter_ == QStringLiteral("edited") && edited)
          || (edit_filter_ == QStringLiteral("unedited") && !edited))) {
        return false;
    }
    const bool liked = sourceModel()->data(row, ReviewModel::LikedRole).toBool();
    return liked_filter_ == QStringLiteral("all")
           || (liked_filter_ == QStringLiteral("liked") && liked)
           || (liked_filter_ == QStringLiteral("unliked") && !liked);
}

bool ReviewFilterModel::lessThan(
    const QModelIndex& source_left,
    const QModelIndex& source_right
) const {
    if (semantic_rank_by_key_.isEmpty()) {
        return QSortFilterProxyModel::lessThan(source_left, source_right);
    }
    const qsizetype left_rank = semantic_rank_by_key_.value(
        semantic_key(*sourceModel(), source_left),
        std::numeric_limits<qsizetype>::max()
    );
    const qsizetype right_rank = semantic_rank_by_key_.value(
        semantic_key(*sourceModel(), source_right),
        std::numeric_limits<qsizetype>::max()
    );
    return left_rank < right_rank;
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

QString ReviewFilterModel::normalizeLikedFilter(const QString& filter) {
    const QString normalized = filter.trimmed().toLower();
    return is_liked_filter(normalized) ? normalized : QStringLiteral("all");
}

QString ReviewFilterModel::normalizeCaptureMonth(const QString& value) {
    const QString normalized = value.trimmed();
    if (normalized.isEmpty()) {
        return {};
    }
    const bool valid =
        normalized.size() == 7 && normalized.at(4) == u'-' && normalized.left(4).toInt() >= 0
        && normalized.mid(5, 2).toInt() >= 1 && normalized.mid(5, 2).toInt() <= 12
        && normalized.left(4).contains(QRegularExpression(QStringLiteral("^[0-9]{4}$")))
        && normalized.mid(5, 2).contains(QRegularExpression(QStringLiteral("^[0-9]{2}$")));
    return valid ? normalized : QString{};
}

QString ReviewFilterModel::normalizeChineseLunarMonthType(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    return is_chinese_lunar_month_type(normalized) ? normalized : QStringLiteral("all");
}

QString ReviewFilterModel::normalizeFacetKey(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    return normalized.size() <= 512 ? normalized : QString{};
}

QStringList ReviewFilterModel::normalizeKeywordIds(const QStringList& values) {
    QStringList normalized;
    QSet<QString> seen;
    for (const QString& value : values) {
        const QString id = value.trimmed();
        if (!id.isEmpty() && !seen.contains(id)) {
            seen.insert(id);
            normalized.push_back(id);
        }
    }
    return normalized;
}

void ReviewFilterModel::refreshRowsFilter() {
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}
