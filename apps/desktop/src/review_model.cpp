#include "review_model.hpp"

#include "review_visual_request.hpp"

#include <QSet>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace {

[[nodiscard]] bool is_decision_flag(const QString& flag) {
    return flag == QStringLiteral("unflagged") || flag == QStringLiteral("picked")
           || flag == QStringLiteral("rejected");
}

[[nodiscard]] bool is_color_label(const QString& color_label) {
    return color_label == QStringLiteral("none") || color_label == QStringLiteral("red")
           || color_label == QStringLiteral("yellow") || color_label == QStringLiteral("green")
           || color_label == QStringLiteral("blue") || color_label == QStringLiteral("purple");
}

[[nodiscard]] bool has_unique_stable_keys(const QVector<ReviewItem>& items) {
    QSet<QString> keys;
    keys.reserve(items.size());
    for (const auto& item : items) {
        if (item.photo_id.isEmpty() || keys.contains(item.photo_id)) {
            return false;
        }
        keys.insert(item.photo_id);
    }
    return true;
}

void append_role(QList<int>& roles, const int role) {
    if (!roles.contains(role)) {
        roles.append(role);
    }
}

[[nodiscard]] QList<int> changed_roles(const ReviewItem& current, const ReviewItem& replacement) {
    QList<int> roles;
    if (current.photo_id != replacement.photo_id) {
        append_role(roles, ReviewModel::PhotoIdRole);
    }
    if (current.representation_id != replacement.representation_id) {
        append_role(roles, ReviewModel::RepresentationIdRole);
    }
    if (current.representation_count != replacement.representation_count) {
        append_role(roles, ReviewModel::RepresentationCountRole);
    }
    if (current.source_location_count != replacement.source_location_count) {
        append_role(roles, ReviewModel::SourceLocationCountRole);
    }
    if (current.has_raw_representation != replacement.has_raw_representation) {
        append_role(roles, ReviewModel::HasRawRepresentationRole);
    }
    if (current.has_raster_representation != replacement.has_raster_representation) {
        append_role(roles, ReviewModel::HasRasterRepresentationRole);
    }
    if (current.location_id != replacement.location_id) {
        append_role(roles, ReviewModel::LocationIdRole);
    }
    if (current.visual_handle != replacement.visual_handle) {
        append_role(roles, ReviewModel::VisualHandleRole);
        append_role(roles, ReviewModel::VisualSourceRole);
    }
    if (current.visual_source_override != replacement.visual_source_override) {
        append_role(roles, ReviewModel::VisualSourceRole);
    }
    if (current.is_remote != replacement.is_remote) {
        append_role(roles, ReviewModel::IsRemoteRole);
    }
    if (current.remote_server_id != replacement.remote_server_id) {
        append_role(roles, ReviewModel::RemoteServerIdRole);
    }
    if (current.remote_photo_id != replacement.remote_photo_id) {
        append_role(roles, ReviewModel::RemotePhotoIdRole);
    }
    if (current.remote_representation_id != replacement.remote_representation_id) {
        append_role(roles, ReviewModel::RemoteRepresentationIdRole);
    }
    if (current.remote_preview_unavailable_reason
        != replacement.remote_preview_unavailable_reason) {
        append_role(roles, ReviewModel::RemotePreviewUnavailableReasonRole);
        append_role(roles, ReviewModel::VisualErrorRole);
    }
    if (current.title != replacement.title) {
        append_role(roles, ReviewModel::TitleRole);
    }
    if (current.source_path != replacement.source_path) {
        append_role(roles, ReviewModel::SourcePathRole);
    }
    if (current.source_available != replacement.source_available) {
        append_role(roles, ReviewModel::SourceAvailableRole);
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
    if (current.has_metadata != replacement.has_metadata)
        append_role(roles, ReviewModel::HasMetadataRole);
    if (current.camera_make != replacement.camera_make)
        append_role(roles, ReviewModel::CameraMakeRole);
    if (current.camera_model != replacement.camera_model)
        append_role(roles, ReviewModel::CameraModelRole);
    if (current.lens_make != replacement.lens_make)
        append_role(roles, ReviewModel::LensMakeRole);
    if (current.lens_model != replacement.lens_model)
        append_role(roles, ReviewModel::LensModelRole);
    if (current.captured_at_unix_seconds != replacement.captured_at_unix_seconds)
        append_role(roles, ReviewModel::CapturedAtUnixSecondsRole);
    if (current.iso_speed != replacement.iso_speed)
        append_role(roles, ReviewModel::IsoSpeedRole);
    if (current.exposure_time_seconds != replacement.exposure_time_seconds)
        append_role(roles, ReviewModel::ExposureTimeSecondsRole);
    if (current.aperture_f_number != replacement.aperture_f_number)
        append_role(roles, ReviewModel::ApertureFNumberRole);
    if (current.focal_length_mm != replacement.focal_length_mm)
        append_role(roles, ReviewModel::FocalLengthMmRole);
    if (current.focal_length_35mm != replacement.focal_length_35mm)
        append_role(roles, ReviewModel::FocalLength35mmRole);
    if (current.raw_width != replacement.raw_width)
        append_role(roles, ReviewModel::RawWidthRole);
    if (current.raw_height != replacement.raw_height)
        append_role(roles, ReviewModel::RawHeightRole);
    if (current.sensor_bits != replacement.sensor_bits)
        append_role(roles, ReviewModel::SensorBitsRole);
    if (current.cfa_pattern != replacement.cfa_pattern)
        append_role(roles, ReviewModel::CfaPatternRole);
    if (current.dng_version != replacement.dng_version)
        append_role(roles, ReviewModel::DngVersionRole);
    if (current.has_technical_observation != replacement.has_technical_observation) {
        append_role(roles, ReviewModel::HasTechnicalObservationRole);
    }
    if (current.technical_input_width != replacement.technical_input_width) {
        append_role(roles, ReviewModel::TechnicalInputWidthRole);
    }
    if (current.technical_input_height != replacement.technical_input_height) {
        append_role(roles, ReviewModel::TechnicalInputHeightRole);
    }
    if (current.technical_preprocessing_version != replacement.technical_preprocessing_version) {
        append_role(roles, ReviewModel::TechnicalPreprocessingVersionRole);
    }
    if (current.technical_implementation_version != replacement.technical_implementation_version) {
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
    if (current.liked != replacement.liked) {
        append_role(roles, ReviewModel::LikedRole);
    }
    if (current.color_label != replacement.color_label) {
        append_role(roles, ReviewModel::ColorLabelRole);
    }
    if (current.library_state_updated_at_ms != replacement.library_state_updated_at_ms) {
        append_role(roles, ReviewModel::LibraryStateUpdatedAtMsRole);
    }
    if (current.has_development_edits != replacement.has_development_edits) {
        append_role(roles, ReviewModel::HasDevelopmentEditsRole);
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
    case RepresentationCountRole:
        return QVariant::fromValue(item.representation_count);
    case SourceLocationCountRole:
        return QVariant::fromValue(item.source_location_count);
    case HasRawRepresentationRole:
        return item.has_raw_representation;
    case HasRasterRepresentationRole:
        return item.has_raster_representation;
    case LocationIdRole:
        return item.location_id;
    case VisualHandleRole:
        return item.visual_handle;
    case TitleRole:
        return item.title;
    case SourcePathRole:
        return item.source_path;
    case SourceAvailableRole:
        return item.source_available;
    case VisualRole:
        return item.visual_role;
    case VisualErrorRole:
        if (item.has_visual) {
            return QString{};
        }
        return item.is_remote && !item.remote_preview_unavailable_reason.isEmpty()
                   ? item.remote_preview_unavailable_reason
                   : QStringLiteral("visual pending");
    case VisualWidthRole:
        return QVariant::fromValue(item.visual_width);
    case VisualHeightRole:
        return QVariant::fromValue(item.visual_height);
    case VisualSourceRole:
        if (!item.has_visual) {
            return QString{};
        }
        if (!item.visual_source_override.isEmpty()) {
            return item.visual_source_override;
        }
        return reviewVisualSource(
            item.visual_handle,
            generation_.load(std::memory_order_acquire),
            ReviewVisualLifetime::Grid
        );
    case HasMetadataRole:
        return item.has_metadata;
    case CameraMakeRole:
        return item.camera_make;
    case CameraModelRole:
        return item.camera_model;
    case LensMakeRole:
        return item.lens_make;
    case LensModelRole:
        return item.lens_model;
    case CapturedAtUnixSecondsRole:
        return QVariant::fromValue(item.captured_at_unix_seconds);
    case IsoSpeedRole:
        return item.iso_speed;
    case ExposureTimeSecondsRole:
        return item.exposure_time_seconds;
    case ApertureFNumberRole:
        return item.aperture_f_number;
    case FocalLengthMmRole:
        return item.focal_length_mm;
    case FocalLength35mmRole:
        return item.focal_length_35mm;
    case RawWidthRole:
        return QVariant::fromValue(item.raw_width);
    case RawHeightRole:
        return QVariant::fromValue(item.raw_height);
    case SensorBitsRole:
        return QVariant::fromValue(item.sensor_bits);
    case CfaPatternRole:
        return item.cfa_pattern;
    case DngVersionRole:
        return item.dng_version;
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
    case LikedRole:
        return item.liked;
    case ColorLabelRole:
        return item.color_label;
    case LibraryStateUpdatedAtMsRole:
        return QVariant::fromValue(item.library_state_updated_at_ms);
    case HasDevelopmentEditsRole:
        return item.has_development_edits;
    case IsRemoteRole:
        return item.is_remote;
    case RemoteServerIdRole:
        return item.remote_server_id;
    case RemotePhotoIdRole:
        return item.remote_photo_id;
    case RemoteRepresentationIdRole:
        return item.remote_representation_id;
    case RemotePreviewUnavailableReasonRole:
        return item.remote_preview_unavailable_reason;
    default:
        return {};
    }
}

QHash<int, QByteArray> ReviewModel::roleNames() const {
    return {
        {PhotoIdRole, "photoId"},
        {RepresentationIdRole, "representationId"},
        {RepresentationCountRole, "representationCount"},
        {SourceLocationCountRole, "sourceLocationCount"},
        {HasRawRepresentationRole, "hasRawRepresentation"},
        {HasRasterRepresentationRole, "hasRasterRepresentation"},
        {LocationIdRole, "locationId"},
        {VisualHandleRole, "visualHandle"},
        {TitleRole, "title"},
        {SourcePathRole, "sourcePath"},
        {SourceAvailableRole, "sourceAvailable"},
        {VisualRole, "visualRole"},
        {VisualErrorRole, "visualError"},
        {VisualWidthRole, "visualWidth"},
        {VisualHeightRole, "visualHeight"},
        {VisualSourceRole, "visualSource"},
        {HasMetadataRole, "hasMetadata"},
        {CameraMakeRole, "cameraMake"},
        {CameraModelRole, "cameraModel"},
        {LensMakeRole, "lensMake"},
        {LensModelRole, "lensModel"},
        {CapturedAtUnixSecondsRole, "capturedAtUnixSeconds"},
        {IsoSpeedRole, "isoSpeed"},
        {ExposureTimeSecondsRole, "exposureTimeSeconds"},
        {ApertureFNumberRole, "apertureFNumber"},
        {FocalLengthMmRole, "focalLengthMm"},
        {FocalLength35mmRole, "focalLength35mm"},
        {RawWidthRole, "rawWidth"},
        {RawHeightRole, "rawHeight"},
        {SensorBitsRole, "sensorBits"},
        {CfaPatternRole, "cfaPattern"},
        {DngVersionRole, "dngVersion"},
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
        {LikedRole, "liked"},
        {ColorLabelRole, "colorLabel"},
        {LibraryStateUpdatedAtMsRole, "libraryStateUpdatedAtMs"},
        {HasDevelopmentEditsRole, "hasDevelopmentEdits"},
        {IsRemoteRole, "isRemote"},
        {RemoteServerIdRole, "remoteServerId"},
        {RemotePhotoIdRole, "remotePhotoId"},
        {RemoteRepresentationIdRole, "remoteRepresentationId"},
        {RemotePreviewUnavailableReasonRole, "remotePreviewUnavailableReason"},
    };
}

void ReviewModel::replace(QVector<ReviewItem> items, const quint64 generation) {
    beginResetModel();
    items_ = std::move(items);
    generation_.store(generation, std::memory_order_release);
    endResetModel();
}

void ReviewModel::setGeneration(const quint64 generation) {
    const quint64 previous = generation_.exchange(generation, std::memory_order_acq_rel);
    if (previous == generation || items_.isEmpty()) {
        return;
    }

    emit dataChanged(
        index(0, 0),
        index(static_cast<int>(items_.size() - 1), 0),
        {VisualSourceRole}
    );
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

bool ReviewModel::appendSnapshot(QVector<ReviewItem> items, const quint64 generation) {
    if (!isGenerationCurrent(generation) || !has_unique_stable_keys(items)
        || !has_unique_stable_keys(items_)) {
        return false;
    }

    QSet<QString> existing_keys;
    existing_keys.reserve(items_.size());
    for (const auto& item : items_) {
        existing_keys.insert(item.photo_id);
    }
    for (const auto& item : items) {
        if (existing_keys.contains(item.photo_id)) {
            return false;
        }
    }
    append(std::move(items));
    return true;
}

bool ReviewModel::reconcileSnapshot(QVector<ReviewItem> items, const quint64 generation) {
    if (!isGenerationCurrent(generation) || !has_unique_stable_keys(items)
        || !has_unique_stable_keys(items_)) {
        return false;
    }

    QSet<QString> desired_keys;
    desired_keys.reserve(items.size());
    for (const auto& item : items) {
        desired_keys.insert(item.photo_id);
    }

    qsizetype row = items_.size();
    while (row > 0) {
        --row;
        if (desired_keys.contains(items_.at(row).photo_id)) {
            continue;
        }

        const qsizetype last = row;
        while (row > 0 && !desired_keys.contains(items_.at(row - 1).photo_id)) {
            --row;
        }
        const qsizetype first = row;
        beginRemoveRows({}, static_cast<int>(first), static_cast<int>(last));
        items_.remove(first, last - first + 1);
        endRemoveRows();
    }

    for (qsizetype target_row = 0; target_row < items.size(); ++target_row) {
        const auto& desired = items.at(target_row);
        if (target_row >= items_.size() || items_.at(target_row).photo_id != desired.photo_id) {
            const auto existing = std::find_if(
                items_.cbegin() + std::min(target_row, items_.size()),
                items_.cend(),
                [&desired](const ReviewItem& candidate) {
                    return candidate.photo_id == desired.photo_id;
                }
            );
            if (existing == items_.cend()) {
                beginInsertRows({}, static_cast<int>(target_row), static_cast<int>(target_row));
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

bool ReviewModel::reconcilePrefixSnapshot(QVector<ReviewItem> items, const quint64 generation) {
    if (!isGenerationCurrent(generation) || !has_unique_stable_keys(items)
        || !has_unique_stable_keys(items_)) {
        return false;
    }

    QSet<QString> prefix_keys;
    prefix_keys.reserve(items.size());
    for (const auto& item : items) {
        prefix_keys.insert(item.photo_id);
    }

    items.reserve(items.size() + items_.size());
    for (const auto& existing : items_) {
        if (!prefix_keys.contains(existing.photo_id)) {
            items.push_back(existing);
        }
    }
    return reconcileSnapshot(std::move(items), generation);
}

bool ReviewModel::replaceRemoteItems(QVector<ReviewItem> items) {
    if (!has_unique_stable_keys(items)
        || std::any_of(items.cbegin(), items.cend(), [](const ReviewItem& item) {
               return !item.is_remote;
           })) {
        return false;
    }

    QSet<QString> local_keys;
    local_keys.reserve(items_.size());
    for (const auto& item : items_) {
        if (!item.is_remote) {
            local_keys.insert(item.photo_id);
        }
    }
    if (std::any_of(items.cbegin(), items.cend(), [&local_keys](const ReviewItem& item) {
            return local_keys.contains(item.photo_id);
        })) {
        return false;
    }

    qsizetype row = items_.size();
    while (row > 0) {
        --row;
        if (!items_.at(row).is_remote) {
            continue;
        }
        const qsizetype last = row;
        while (row > 0 && items_.at(row - 1).is_remote) {
            --row;
        }
        const qsizetype first = row;
        beginRemoveRows({}, static_cast<int>(first), static_cast<int>(last));
        items_.remove(first, last - first + 1);
        endRemoveRows();
    }
    append(std::move(items));
    return true;
}

bool ReviewModel::isGenerationCurrent(const quint64 generation) const noexcept {
    return generation_.load(std::memory_order_acquire) == generation;
}

QString ReviewModel::visualSourceFor(const QString& ticket) const {
    return reviewVisualSource(
        ticket,
        generation_.load(std::memory_order_acquire),
        ReviewVisualLifetime::Comparison
    );
}

std::optional<ReviewDecisionValue> ReviewModel::decisionFor(const QString& photo_id) const {
    const auto item =
        std::find_if(items_.cbegin(), items_.cend(), [&photo_id](const ReviewItem& candidate) {
            return candidate.photo_id == photo_id;
        });
    if (item == items_.cend()) {
        return std::nullopt;
    }
    return ReviewDecisionValue{
        .head_sequence = item->decision_head_sequence,
        .flag = item->decision_flag,
        .rating = item->decision_rating,
    };
}

std::optional<ReviewLibraryStateValue> ReviewModel::libraryStateFor(const QString& photo_id) const {
    const auto item =
        std::find_if(items_.cbegin(), items_.cend(), [&photo_id](const ReviewItem& candidate) {
            return candidate.photo_id == photo_id;
        });
    if (item == items_.cend()) {
        return std::nullopt;
    }
    return ReviewLibraryStateValue{
        .liked = item->liked,
        .color_label = item->color_label,
        .updated_at_ms = item->library_state_updated_at_ms,
    };
}

QVector<ReviewLocalSourceProbe> ReviewModel::localSourceProbes() const {
    QVector<ReviewLocalSourceProbe> probes;
    probes.reserve(items_.size());
    for (const auto& item : items_) {
        if (item.is_remote || item.photo_id.isEmpty() || item.location_id.isEmpty()
            || item.source_path.isEmpty()) {
            continue;
        }
        probes.push_back({
            .photo_id = item.photo_id,
            .location_id = item.location_id,
            .source_path = item.source_path,
        });
    }
    return probes;
}

bool ReviewModel::applyLocalSourceAvailability(
    const QVector<ReviewLocalSourceAvailability>& observations
) {
    QHash<QString, ReviewLocalSourceAvailability> by_photo;
    by_photo.reserve(observations.size());
    for (const auto& observation : observations) {
        if (observation.source.photo_id.isEmpty() || observation.source.location_id.isEmpty()
            || observation.source.source_path.isEmpty()) {
            continue;
        }
        by_photo.insert(observation.source.photo_id, observation);
    }

    bool any_changed = false;
    for (qsizetype row = 0; row < items_.size(); ++row) {
        auto& item = items_[row];
        if (item.is_remote) {
            continue;
        }
        const auto observation = by_photo.constFind(item.photo_id);
        if (observation == by_photo.cend() || observation->source.location_id != item.location_id
            || observation->source.source_path != item.source_path
            || observation->available == item.source_available) {
            continue;
        }
        item.source_available = observation->available;
        const QModelIndex changed = index(static_cast<int>(row), 0);
        emit dataChanged(changed, changed, {SourceAvailableRole});
        emit localSourceAvailabilityChanged(item.photo_id, item.source_available);
        any_changed = true;
    }
    return any_changed;
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

bool ReviewModel::updateLibraryState(
    const QString& photo_id,
    const bool liked,
    const QString& color_label,
    const std::int64_t updated_at_ms
) {
    if (photo_id.isEmpty() || !is_color_label(color_label)) {
        return false;
    }
    bool any_changed = false;
    for (qsizetype row = 0; row < items_.size(); ++row) {
        auto& item = items_[row];
        if (item.photo_id != photo_id) {
            continue;
        }
        const bool state_changed = item.liked != liked || item.color_label != color_label
                                   || item.library_state_updated_at_ms != updated_at_ms;
        if (!state_changed) {
            continue;
        }
        item.liked = liked;
        item.color_label = color_label;
        item.library_state_updated_at_ms = updated_at_ms;
        const QModelIndex changed_index = index(static_cast<int>(row), 0);
        emit dataChanged(
            changed_index,
            changed_index,
            {LikedRole, ColorLabelRole, LibraryStateUpdatedAtMsRole}
        );
        any_changed = true;
    }
    return any_changed;
}
