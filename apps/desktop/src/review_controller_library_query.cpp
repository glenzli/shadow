#include "review_controller.hpp"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <exception>

// Library import, paging, range projection, and filter-query routing.

namespace {

QVariantMap selectionTarget(const QAbstractItemModel& model, const int row) {
    const QModelIndex index = model.index(row, 0);
    QVariantMap result;
    const auto roles = model.roleNames();
    for (auto role = roles.cbegin(); role != roles.cend(); ++role) {
        result.insert(QString::fromUtf8(role.value()), model.data(index, role.key()));
    }
    return result;
}

} // namespace

void ReviewController::scanFolder(const QUrl& folder_url) {
    const bool admitted = !scanning() && !query_coordinator_.pageRunning()
                          && !query_coordinator_.refreshing() && !comparison_coordinator_.busy()
                          && !decision_coordinator_.busy();
    if (!import_coordinator_.start(folder_url, admitted)) {
        if (!import_coordinator_.statusMessage().isEmpty()) {
            setStatusMessage(import_coordinator_.statusMessage());
        }
        return;
    }
    query_coordinator_.clearForImportStart();
}

void ReviewController::refreshSelectedPreviews(const QVariantList& targets) {
    const bool admitted = !scanning() && !query_coordinator_.pageRunning()
                          && !query_coordinator_.refreshing() && !comparison_coordinator_.busy()
                          && !decision_coordinator_.busy();
    if (!admitted || targets.isEmpty()) {
        return;
    }

    QString selected_folder;
    QStringList visual_handles;
    visual_handles.reserve(targets.size());
    for (const QVariant& value : targets) {
        const QVariantMap target = value.toMap();
        const QString source_path = target.value(QStringLiteral("sourcePath")).toString();
        const QString visual_handle = target.value(QStringLiteral("visualHandle")).toString();
        if (source_path.isEmpty() || visual_handle.isEmpty()
            || !target.value(QStringLiteral("sourceAvailable"), true).toBool()
            || target.value(QStringLiteral("isRemote")).toBool()) {
            setStatusMessage({
                "ReviewController",
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Preview refresh requires selected local photos with generated proxies"
                ),
            });
            return;
        }
        const QString folder = QFileInfo(source_path).absolutePath();
        if (folder.isEmpty() || (!selected_folder.isEmpty() && folder != selected_folder)) {
            setStatusMessage({
                "ReviewController",
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Refresh selected previews from one source folder at a time"
                ),
            });
            return;
        }
        selected_folder = folder;
        visual_handles.push_back(visual_handle);
    }

    try {
        const std::uint32_t invalidated = backend_->refreshSelectedReviewPreviews(visual_handles);
        if (invalidated == 0) {
            setStatusMessage({
                "ReviewController",
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Selected previews have already been replaced"
                ),
            });
            return;
        }
    } catch (const std::exception&) {
        setStatusMessage({
            "ReviewController",
            QT_TRANSLATE_NOOP("ReviewController", "Could not refresh the selected previews"),
        });
        return;
    }
    scanFolder(QUrl::fromLocalFile(selected_folder));
}

void ReviewController::cancelScan() {
    static_cast<void>(import_coordinator_.cancel());
}

void ReviewController::loadMore() {
    const bool admitted =
        !scanning() && !comparison_coordinator_.busy() && !decision_coordinator_.busy();
    static_cast<void>(query_coordinator_.loadMore(admitted));
}

QVariantList ReviewController::currentSelectionTargets(const QVariantList& targets) const {
    QSet<QPair<QString, QString>> identities;
    for (const QVariant& value : targets) {
        const QVariantMap target = value.toMap();
        identities.insert(
            {target.value(QStringLiteral("photoId")).toString(),
             target.value(QStringLiteral("representationId")).toString()}
        );
    }
    QVariantList current;
    for (int row = 0; row < filtered_model_.rowCount() && !identities.isEmpty(); ++row) {
        const QModelIndex index = filtered_model_.index(row, 0);
        const QPair<QString, QString> identity{
            filtered_model_.data(index, ReviewModel::PhotoIdRole).toString(),
            filtered_model_.data(index, ReviewModel::RepresentationIdRole).toString(),
        };
        if (identities.remove(identity)) {
            current.push_back(selectionTarget(filtered_model_, row));
        }
    }
    return current;
}

QVariantList ReviewController::selectionRangeTargets(
    const QString& anchor_photo_id,
    const QString& anchor_representation_id,
    const QString& photo_id,
    const QString& representation_id
) const {
    if (anchor_photo_id.isEmpty() || anchor_representation_id.isEmpty() || photo_id.isEmpty()
        || representation_id.isEmpty()) {
        return {};
    }

    int anchor_row = -1;
    int target_row = -1;
    const int count = filtered_model_.rowCount();
    for (int row = 0; row < count && (anchor_row < 0 || target_row < 0); ++row) {
        const QModelIndex index = filtered_model_.index(row, 0);
        const QString current_photo_id =
            filtered_model_.data(index, ReviewModel::PhotoIdRole).toString();
        const QString current_representation_id =
            filtered_model_.data(index, ReviewModel::RepresentationIdRole).toString();
        if (current_photo_id == anchor_photo_id
            && current_representation_id == anchor_representation_id) {
            anchor_row = row;
        }
        if (current_photo_id == photo_id && current_representation_id == representation_id) {
            target_row = row;
        }
    }
    if (anchor_row < 0 || target_row < 0) {
        return {};
    }

    const int first = std::min(anchor_row, target_row);
    const int last = std::max(anchor_row, target_row);
    QVariantList targets;
    targets.reserve(last - first + 1);
    for (int row = first; row <= last; ++row) {
        targets.push_back(selectionTarget(filtered_model_, row));
    }
    return targets;
}

void ReviewController::clearFilters() {
    availability_filter_settings_->setValue(QStringLiteral("library/hide_offline_uncached"), false);
    availability_filter_settings_->setValue(QStringLiteral("library/only_editable"), false);
    filtered_model_.clearFilters();
    album_coordinator_.clearAlbumSelection();
    emit allFiltersCleared();
}

void ReviewController::setSemanticRepresentationOrder(const QStringList& ranked_keys) {
    filtered_model_.setSemanticRepresentationOrder(ranked_keys);
}

void ReviewController::setSmartCategoryRepresentationKeys(const QStringList& member_keys) {
    filtered_model_.setSmartCategoryRepresentationKeys(member_keys);
}

void ReviewController::refreshVisibleLibrary() {
    if (scanning() || decision_coordinator_.busy()) {
        return;
    }
    requestLibraryReset();
}

void ReviewController::setFilterFlag(const QString& filter) {
    filtered_model_.setFlagFilter(filter);
}

void ReviewController::setFilterMinimumRating(const int rating) {
    filtered_model_.setMinimumRating(rating);
}

void ReviewController::setFilterColorLabel(const QString& color_label) {
    filtered_model_.setColorFilter(color_label);
}

void ReviewController::setFilterEditState(const QString& edit_state) {
    filtered_model_.setEditFilter(edit_state);
}

void ReviewController::setFilterHideOfflineUncached(const bool enabled) {
    availability_filter_settings_->setValue(
        QStringLiteral("library/hide_offline_uncached"),
        enabled
    );
    filtered_model_.setHideOfflineUncached(enabled);
}

void ReviewController::setFilterOnlyEditable(const bool enabled) {
    availability_filter_settings_->setValue(QStringLiteral("library/only_editable"), enabled);
    filtered_model_.setOnlyEditable(enabled);
}

void ReviewController::setFilterLiked(const QString& liked) {
    filtered_model_.setLikedFilter(liked);
}

void ReviewController::setLibrarySortKey(const QString& sort_key) {
    const QString normalized = sort_key.trimmed().toLower();
    if ((normalized != QStringLiteral("capture_time") && normalized != QStringLiteral("name"))
        || library_sort_key_ == normalized) {
        return;
    }
    library_sort_key_ = normalized;
    model_.setPresentationOrder(
        normalized == QStringLiteral("name") ? ReviewModel::PresentationSortKey::Name
                                             : ReviewModel::PresentationSortKey::CaptureTime,
        library_sort_descending_
    );
    emit libraryOrderChanged();
    requestLibraryReset();
}

void ReviewController::setLibrarySortDescending(const bool descending) {
    if (library_sort_descending_ == descending) {
        return;
    }
    library_sort_descending_ = descending;
    model_.setPresentationOrder(
        library_sort_key_ == QStringLiteral("name") ? ReviewModel::PresentationSortKey::Name
                                                    : ReviewModel::PresentationSortKey::CaptureTime,
        descending
    );
    emit libraryOrderChanged();
    requestLibraryReset();
}

void ReviewController::setFilterExcludedFlag(const QString& flag) {
    filtered_model_.setExcludedFlagFilter(flag);
}

void ReviewController::setFilterExcludedColorLabel(const QString& color_label) {
    filtered_model_.setExcludedColorFilter(color_label);
}

void ReviewController::setFilterCaptureMonth(const QString& capture_month) {
    filtered_model_.setCaptureMonth(capture_month);
}

void ReviewController::setFilterChineseLunarMonth(const int month) {
    filtered_model_.setChineseLunarMonth(month);
}

void ReviewController::setFilterChineseLunarDay(const int day) {
    filtered_model_.setChineseLunarDay(day);
}

void ReviewController::setFilterChineseLunarMonthType(const QString& month_type) {
    filtered_model_.setChineseLunarMonthType(month_type);
}

void ReviewController::setFilterCameraKey(const QString& camera_key) {
    filtered_model_.setCameraKey(camera_key);
}

void ReviewController::setFilterLensKey(const QString& lens_key) {
    filtered_model_.setLensKey(lens_key);
}

void ReviewController::setFilterCountryKey(const QString& country_key) {
    const QString previous = filtered_model_.countryKey();
    filtered_model_.setCountryKey(country_key);
    if (filtered_model_.countryKey() != previous && !filtered_model_.localityKey().isEmpty()) {
        filtered_model_.setLocalityKey({});
    }
}

void ReviewController::setFilterLocalityKey(const QString& locality_key) {
    filtered_model_.setLocalityKey(locality_key);
}

void ReviewController::setTravelFilterEnabled(const bool enabled) {
    filtered_model_.setTravelFilterEnabled(enabled && !travel_living_place_rules_.isEmpty());
}

void ReviewController::setDailyFilterEnabled(const bool enabled) {
    filtered_model_.setDailyFilterEnabled(enabled && !travel_living_place_rules_.isEmpty());
}

void ReviewController::setTravelLivingPlaces(const QVariantList& living_places) {
    static const QRegularExpression month_pattern(QStringLiteral("^[0-9]{4}-(0[1-9]|1[0-2])$"));
    QVector<BackendLibraryLivingPlaceRule> normalized;
    normalized.reserve(std::min<qsizetype>(living_places.size(), 32));
    for (const QVariant& value : living_places.mid(0, 32)) {
        const QVariantMap place = value.toMap();
        BackendLibraryLivingPlaceRule rule{
            .locality_key = place.value(QStringLiteral("key")).toString().trimmed().toLower(),
            .start_month = place.value(QStringLiteral("startMonth")).toString().trimmed(),
            .end_month = place.value(QStringLiteral("endMonth")).toString().trimmed(),
        };
        if (rule.locality_key.isEmpty()
            || (!rule.start_month.isEmpty() && !month_pattern.match(rule.start_month).hasMatch())
            || (!rule.end_month.isEmpty() && !month_pattern.match(rule.end_month).hasMatch())
            || (!rule.start_month.isEmpty() && !rule.end_month.isEmpty()
                && rule.start_month > rule.end_month)) {
            continue;
        }
        if (!normalized.contains(rule)) {
            normalized.push_back(std::move(rule));
        }
    }
    if (travel_living_place_rules_ == normalized) {
        return;
    }
    travel_living_place_rules_ = std::move(normalized);
    if (travel_living_place_rules_.isEmpty()) {
        filtered_model_.setTravelFilterEnabled(false);
        filtered_model_.setDailyFilterEnabled(false);
    } else if (filtered_model_.travelFilterEnabled() || filtered_model_.dailyFilterEnabled()) {
        requestLibraryReset();
    }
    refreshTravelCollections();
}

void ReviewController::setFilterKeywordIdsAll(const QStringList& keyword_ids) {
    filtered_model_.setKeywordIdsAll(keyword_ids);
}

void ReviewController::setFilterExcludedKeywordIdsAny(const QStringList& keyword_ids) {
    filtered_model_.setExcludedKeywordIdsAny(keyword_ids);
}

void ReviewController::setLibraryAlbumId(const QString& album_id) {
    album_coordinator_.setAlbumId(album_id);
}

void ReviewController::requestLibraryReset() {
    query_coordinator_.requestReset(currentLibraryFilter(), currentLibraryOrder());
}

void ReviewController::scheduleFilterQuery() {
    query_coordinator_.scheduleReset(currentLibraryFilter(), currentLibraryOrder());
}

BackendLibraryPhotoOrder ReviewController::currentLibraryOrder() const noexcept {
    if (library_sort_key_ == QStringLiteral("name")) {
        return library_sort_descending_ ? BackendLibraryPhotoOrder::FileNameDescending
                                        : BackendLibraryPhotoOrder::FileNameAscending;
    }
    return library_sort_descending_ ? BackendLibraryPhotoOrder::CaptureTimeDescending
                                    : BackendLibraryPhotoOrder::CaptureTimeAscending;
}

BackendLibraryPhotoFilter ReviewController::currentLibraryFilter() const {
    BackendLibraryPhotoFilter filter;
    const QString flag = filtered_model_.flagFilter();
    if (flag == QStringLiteral("unflagged")) {
        filter.flag = BackendLibraryFlagFilter::Unflagged;
    } else if (flag == QStringLiteral("picked")) {
        filter.flag = BackendLibraryFlagFilter::Picked;
    } else if (flag == QStringLiteral("rejected")) {
        filter.flag = BackendLibraryFlagFilter::Rejected;
    }

    const int minimum_rating = filtered_model_.minimumRating();
    if (minimum_rating > 0) {
        filter.has_minimum_rating = true;
        filter.minimum_rating = static_cast<std::uint8_t>(minimum_rating);
    }

    const QString color = filtered_model_.colorFilter();
    if (color != QStringLiteral("all")) {
        filter.color_label = color;
    }

    const QString edit = filtered_model_.editFilter();
    if (edit == QStringLiteral("edited")) {
        filter.has_development_edits = true;
        filter.development_edits = true;
    } else if (edit == QStringLiteral("unedited")) {
        filter.has_development_edits = true;
        filter.development_edits = false;
    }

    const QString liked = filtered_model_.likedFilter();
    if (liked == QStringLiteral("liked")) {
        filter.has_liked = true;
        filter.liked = true;
    } else if (liked == QStringLiteral("unliked")) {
        filter.has_liked = true;
        filter.liked = false;
    }
    filter.capture_month = filtered_model_.captureMonth();
    const int chinese_lunar_month = filtered_model_.chineseLunarMonth();
    if (chinese_lunar_month > 0) {
        filter.has_chinese_lunar_month = true;
        filter.chinese_lunar_month = static_cast<std::uint8_t>(chinese_lunar_month);
    }
    const int chinese_lunar_day = filtered_model_.chineseLunarDay();
    if (chinese_lunar_day > 0) {
        filter.has_chinese_lunar_day = true;
        filter.chinese_lunar_day = static_cast<std::uint8_t>(chinese_lunar_day);
    }
    const QString chinese_lunar_month_type = filtered_model_.chineseLunarMonthType();
    if (chinese_lunar_month_type == QStringLiteral("regular")) {
        filter.has_chinese_lunar_is_leap_month = true;
        filter.chinese_lunar_is_leap_month = false;
    } else if (chinese_lunar_month_type == QStringLiteral("leap")) {
        filter.has_chinese_lunar_is_leap_month = true;
        filter.chinese_lunar_is_leap_month = true;
    }
    filter.camera_key = filtered_model_.cameraKey();
    filter.lens_key = filtered_model_.lensKey();
    filter.country_key = filtered_model_.countryKey();
    filter.locality_key = filtered_model_.localityKey();
    if (filtered_model_.travelFilterEnabled() || filtered_model_.dailyFilterEnabled()) {
        filter.living_place_rules = travel_living_place_rules_;
        filter.include_living_place_rules = filtered_model_.dailyFilterEnabled();
    }
    filter.album_id = album_coordinator_.albumId();
    filter.keyword_ids_all = filtered_model_.keywordIdsAll();
    filter.excluded_keyword_ids_any = filtered_model_.excludedKeywordIdsAny();
    return filter;
}
