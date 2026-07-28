#include "review_controller.hpp"

#include <algorithm>

// Library import, paging, range projection, and filter-query routing.

void ReviewController::scanFolder(const QUrl& folder_url) {
    const bool admitted = !scanning() && !query_coordinator_.pageRunning()
        && !query_coordinator_.refreshing()
        && !comparison_coordinator_.busy()
        && !decision_coordinator_.busy();
    if (!import_coordinator_.start(folder_url, admitted)) {
        if (!import_coordinator_.statusMessage().isEmpty()) {
            setStatusMessage(import_coordinator_.statusMessage());
        }
        return;
    }
    query_coordinator_.clearForImportStart();
}

void ReviewController::cancelScan() {
    static_cast<void>(import_coordinator_.cancel());
}

void ReviewController::loadMore() {
    const bool admitted = !scanning() && !comparison_coordinator_.busy()
        && !decision_coordinator_.busy();
    static_cast<void>(query_coordinator_.loadMore(admitted));
}

QVariantList ReviewController::selectionRangeTargets(
    const QString& anchor_photo_id,
    const QString& anchor_representation_id,
    const QString& photo_id,
    const QString& representation_id
) const {
    if (anchor_photo_id.isEmpty() || anchor_representation_id.isEmpty()
        || photo_id.isEmpty() || representation_id.isEmpty()) {
        return {};
    }

    int anchor_row = -1;
    int target_row = -1;
    const int count = filtered_model_.rowCount();
    for (int row = 0; row < count && (anchor_row < 0 || target_row < 0);
         ++row) {
        const QModelIndex index = filtered_model_.index(row, 0);
        const QString current_photo_id =
            filtered_model_.data(index, ReviewModel::PhotoIdRole).toString();
        const QString current_representation_id =
            filtered_model_.data(
                index,
                ReviewModel::RepresentationIdRole
            ).toString();
        if (current_photo_id == anchor_photo_id
            && current_representation_id == anchor_representation_id) {
            anchor_row = row;
        }
        if (current_photo_id == photo_id
            && current_representation_id == representation_id) {
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
        const QModelIndex index = filtered_model_.index(row, 0);
        targets.push_back(QVariantMap{
            {QStringLiteral("photoId"),
             filtered_model_.data(
                 index,
                 ReviewModel::PhotoIdRole
             ).toString()},
            {QStringLiteral("representationId"),
             filtered_model_.data(
                 index,
                 ReviewModel::RepresentationIdRole
             ).toString()},
            {QStringLiteral("sourcePath"),
             filtered_model_.data(
                 index,
                 ReviewModel::SourcePathRole
             ).toString()},
            {QStringLiteral("title"),
             filtered_model_.data(
                 index,
                 ReviewModel::TitleRole
             ).toString()},
        });
    }
    return targets;
}

void ReviewController::clearFilters() {
    filtered_model_.clearFilters();
    album_coordinator_.clearAlbumSelection();
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

void ReviewController::setFilterLiked(const QString& liked) {
    filtered_model_.setLikedFilter(liked);
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

void ReviewController::setFilterCameraKey(const QString& camera_key) {
    filtered_model_.setCameraKey(camera_key);
}

void ReviewController::setFilterLensKey(const QString& lens_key) {
    filtered_model_.setLensKey(lens_key);
}

void ReviewController::setLibraryAlbumId(const QString& album_id) {
    album_coordinator_.setAlbumId(album_id);
}

void ReviewController::requestLibraryReset() {
    query_coordinator_.requestReset(currentLibraryFilter());
}

void ReviewController::scheduleFilterQuery() {
    query_coordinator_.scheduleReset(currentLibraryFilter());
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
    filter.camera_key = filtered_model_.cameraKey();
    filter.lens_key = filtered_model_.lensKey();
    filter.album_id = album_coordinator_.albumId();
    return filter;
}
