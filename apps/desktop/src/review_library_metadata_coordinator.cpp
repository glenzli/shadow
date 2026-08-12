#include "review_library_metadata_coordinator.hpp"

#include <QtConcurrentRun>

#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] QVariantMap metadata_presentation(
    const BackendLibraryMetadataState& source
) {
    if (source.photo_id.isEmpty()) {
        return {};
    }
    constexpr double e7_scale = 10'000'000.0;
    return {
        {QStringLiteral("photoId"), source.photo_id},
        {
            QStringLiteral("hasObservedCaptureTime"),
            source.has_observed_capture_time,
        },
        {
            QStringLiteral("observedCapturedAtUnixSeconds"),
            source.observed_captured_at_unix_seconds,
        },
        {
            QStringLiteral("hasEffectiveCaptureTime"),
            source.has_effective_capture_time,
        },
        {
            QStringLiteral("effectiveCapturedAtUnixSeconds"),
            source.effective_captured_at_unix_seconds,
        },
        {
            QStringLiteral("captureTimeOverrideMode"),
            source.capture_time_override_mode,
        },
        {
            QStringLiteral("captureTimeOverrideOrigin"),
            source.capture_time_override_origin,
        },
        {
            QStringLiteral("captureTimeSourceLabel"),
            source.capture_time_source_label,
        },
        {
            QStringLiteral("hasObservedCoordinates"),
            source.has_observed_coordinates,
        },
        {
            QStringLiteral("observedLatitude"),
            static_cast<double>(source.observed_latitude_e7) / e7_scale,
        },
        {
            QStringLiteral("observedLongitude"),
            static_cast<double>(source.observed_longitude_e7) / e7_scale,
        },
        {
            QStringLiteral("hasEffectiveCoordinates"),
            source.has_effective_coordinates,
        },
        {
            QStringLiteral("effectiveLatitude"),
            static_cast<double>(source.effective_latitude_e7) / e7_scale,
        },
        {
            QStringLiteral("effectiveLongitude"),
            static_cast<double>(source.effective_longitude_e7) / e7_scale,
        },
        {
            QStringLiteral("effectivePlaceName"),
            source.effective_place_name,
        },
        {
            QStringLiteral("coordinatesOverrideMode"),
            source.coordinates_override_mode,
        },
        {
            QStringLiteral("coordinatesOverrideOrigin"),
            source.coordinates_override_origin,
        },
        {
            QStringLiteral("coordinatesSourceLabel"),
            source.coordinates_source_label,
        },
    };
}

[[nodiscard]] QVariantMap gpx_preview_presentation(
    const BackendGpxImportPreview& source
) {
    if (source.preview_id.isEmpty()) {
        return {};
    }
    QVariantList sample;
    sample.reserve(source.proposal_sample.size());
    constexpr double e7_scale = 10'000'000.0;
    for (const auto& proposal : source.proposal_sample) {
        sample.push_back(QVariantMap{
            {QStringLiteral("photoId"), proposal.photo_id},
            {
                QStringLiteral("capturedAtUnixSeconds"),
                proposal.captured_at_unix_seconds,
            },
            {
                QStringLiteral("matchedAtUnixSeconds"),
                proposal.matched_at_unix_seconds,
            },
            {
                QStringLiteral("nearestTrackDeltaSeconds"),
                proposal.nearest_track_delta_seconds,
            },
            {
                QStringLiteral("latitude"),
                static_cast<double>(proposal.latitude_e7) / e7_scale,
            },
            {
                QStringLiteral("longitude"),
                static_cast<double>(proposal.longitude_e7) / e7_scale,
            },
        });
    }
    return {
        {QStringLiteral("previewId"), source.preview_id},
        {QStringLiteral("sourcePath"), source.source_path},
        {QStringLiteral("sourceDigestHex"), source.source_digest_hex},
        {
            QStringLiteral("requestedPhotoCount"),
            source.requested_photo_count,
        },
        {QStringLiteral("matchedPhotoCount"), source.matched_photo_count},
        {
            QStringLiteral("unmatchedPhotoCount"),
            source.unmatched_photo_count,
        },
        {QStringLiteral("proposalSample"), sample},
    };
}

[[nodiscard]] QVariantMap capture_time_preview_presentation(
    const BackendCaptureTimeBatchPreview& source
) {
    if (source.preview_id.isEmpty()) {
        return {};
    }
    QVariantList sample;
    sample.reserve(source.proposal_sample.size());
    for (const auto& proposal : source.proposal_sample) {
        sample.push_back(QVariantMap{
            {QStringLiteral("photoId"), proposal.photo_id},
            {
                QStringLiteral("hasBeforeCaptureTime"),
                proposal.has_before_capture_time,
            },
            {
                QStringLiteral("beforeCapturedAtUnixSeconds"),
                proposal.before_captured_at_unix_seconds,
            },
            {
                QStringLiteral("hasAfterCaptureTime"),
                proposal.has_after_capture_time,
            },
            {
                QStringLiteral("afterCapturedAtUnixSeconds"),
                proposal.after_captured_at_unix_seconds,
            },
        });
    }
    return {
        {QStringLiteral("previewId"), source.preview_id},
        {QStringLiteral("mode"), source.mode},
        {QStringLiteral("offsetSeconds"), source.offset_seconds},
        {
            QStringLiteral("requestedPhotoCount"),
            source.requested_photo_count,
        },
        {
            QStringLiteral("applicablePhotoCount"),
            source.applicable_photo_count,
        },
        {
            QStringLiteral("skippedPhotoCount"),
            source.skipped_photo_count,
        },
        {QStringLiteral("proposalSample"), sample},
    };
}

[[nodiscard]] QVariantMap coordinate_batch_preview_presentation(
    const BackendCoordinateBatchPreview& source
) {
    if (source.preview_id.isEmpty()) {
        return {};
    }
    constexpr double e7_scale = 10'000'000.0;
    return {
        {QStringLiteral("previewId"), source.preview_id},
        {QStringLiteral("mode"), source.mode},
        {
            QStringLiteral("latitude"),
            static_cast<double>(source.latitude_e7) / e7_scale,
        },
        {
            QStringLiteral("longitude"),
            static_cast<double>(source.longitude_e7) / e7_scale,
        },
        {QStringLiteral("placeName"), source.place_name},
        {
            QStringLiteral("requestedPhotoCount"),
            source.requested_photo_count,
        },
        {
            QStringLiteral("applicablePhotoCount"),
            source.applicable_photo_count,
        },
        {QStringLiteral("skippedPhotoCount"), source.skipped_photo_count},
        {QStringLiteral("missingPhotoCount"), source.missing_photo_count},
        {QStringLiteral("existingPhotoCount"), source.existing_photo_count},
        {
            QStringLiteral("replacementPhotoCount"),
            source.replacement_photo_count,
        },
    };
}

[[nodiscard]] QVariantMap batch_receipt_presentation(
    const BackendLibraryMetadataBatchReceipt& source
) {
    if (source.requested_photo_count == 0) {
        return {};
    }
    return {
        {
            QStringLiteral("requestedPhotoCount"),
            source.requested_photo_count,
        },
        {QStringLiteral("appliedPhotoCount"), source.applied_photo_count},
        {
            QStringLiteral("skippedPhotoCount"),
            source.requested_photo_count - source.applied_photo_count,
        },
    };
}

} // namespace

ReviewLibraryMetadataCoordinator::ReviewLibraryMetadataCoordinator(
    Operations operations,
    QObject* parent
)
    : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.load || !operations_.set_capture_time
        || !operations_.set_coordinates || !operations_.preview_capture_time
        || !operations_.apply_capture_time || !operations_.preview_coordinates
        || !operations_.apply_coordinates || !operations_.preview_gpx
        || !operations_.apply_gpx) {
        throw std::invalid_argument(
            "Library metadata coordinator requires complete operations"
        );
    }
    connect(
        &watcher_,
        &QFutureWatcher<ReviewLibraryMetadataTaskResult>::finished,
        this,
        &ReviewLibraryMetadataCoordinator::finish
    );
}

ReviewLibraryMetadataCoordinator::~ReviewLibraryMetadataCoordinator() {
    watcher_.waitForFinished();
}

QVariantMap ReviewLibraryMetadataCoordinator::metadata() const {
    return metadata_presentation(metadata_);
}

QVariantMap ReviewLibraryMetadataCoordinator::captureTimePreview() const {
    return capture_time_preview_presentation(capture_time_preview_);
}

QVariantMap ReviewLibraryMetadataCoordinator::coordinateBatchPreview() const {
    return coordinate_batch_preview_presentation(coordinate_batch_preview_);
}

QVariantMap ReviewLibraryMetadataCoordinator::gpxPreview() const {
    return gpx_preview_presentation(gpx_preview_);
}

QVariantMap ReviewLibraryMetadataCoordinator::batchReceipt() const {
    return batch_receipt_presentation(batch_receipt_);
}

bool ReviewLibraryMetadataCoordinator::busy() const noexcept {
    return running_;
}

QString ReviewLibraryMetadataCoordinator::statusCode() const {
    return status_code_;
}

QString ReviewLibraryMetadataCoordinator::errorText() const {
    return error_;
}

void ReviewLibraryMetadataCoordinator::request(const QString& photo_id) {
    if (photo_id.isEmpty()) {
        clear();
        return;
    }
    start(
        ReviewLibraryMetadataTaskResult::Kind::Load,
        QStringLiteral("loading"),
        [operations = operations_, photo_id]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind = ReviewLibraryMetadataTaskResult::Kind::Load;
            result.metadata = operations.load(photo_id);
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::clear() {
    if (running_) {
        return;
    }
    metadata_ = {};
    capture_time_preview_ = {};
    coordinate_batch_preview_ = {};
    gpx_preview_ = {};
    batch_receipt_ = {};
    status_code_ = QStringLiteral("idle");
    error_.clear();
    emit stateChanged();
}

void ReviewLibraryMetadataCoordinator::setCaptureTime(
    const QString& photo_id,
    const QString& mode,
    const std::int64_t captured_at_unix_seconds
) {
    start(
        ReviewLibraryMetadataTaskResult::Kind::CaptureMutation,
        QStringLiteral("saving"),
        [operations = operations_, photo_id, mode, captured_at_unix_seconds]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind =
                ReviewLibraryMetadataTaskResult::Kind::CaptureMutation;
            result.metadata = operations.set_capture_time(
                photo_id,
                mode,
                captured_at_unix_seconds
            );
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::setCoordinates(
    const QString& photo_id,
    const QString& mode,
    const double latitude_degrees,
    const double longitude_degrees,
    const QString& place_name
) {
    start(
        ReviewLibraryMetadataTaskResult::Kind::CoordinatesMutation,
        QStringLiteral("saving"),
        [
            operations = operations_,
            photo_id,
            mode,
            latitude_degrees,
            longitude_degrees,
            place_name
        ]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind =
                ReviewLibraryMetadataTaskResult::Kind::CoordinatesMutation;
            result.metadata = operations.set_coordinates(
                photo_id,
                mode,
                latitude_degrees,
                longitude_degrees,
                place_name
            );
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::previewCaptureTime(
    const QVariantList& targets,
    const QString& mode,
    const std::int64_t offset_seconds
) {
    const QVector<BackendBatchPhotoTarget> batch = batchTargets(targets);
    start(
        ReviewLibraryMetadataTaskResult::Kind::CaptureBatchPreview,
        QStringLiteral("previewing"),
        [operations = operations_, batch, mode, offset_seconds]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind =
                ReviewLibraryMetadataTaskResult::Kind::CaptureBatchPreview;
            result.capture_time_preview = operations.preview_capture_time(
                batch,
                mode,
                offset_seconds
            );
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::applyCaptureTime(
    const QString& preview_id
) {
    start(
        ReviewLibraryMetadataTaskResult::Kind::CaptureBatchApply,
        QStringLiteral("applying"),
        [operations = operations_, preview_id]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind =
                ReviewLibraryMetadataTaskResult::Kind::CaptureBatchApply;
            result.batch_receipt =
                operations.apply_capture_time(preview_id);
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::previewCoordinates(
    const QVariantList& targets,
    const QString& mode,
    const double latitude_degrees,
    const double longitude_degrees,
    const QString& place_name,
    const QString& source_label
) {
    const QVector<BackendBatchPhotoTarget> batch = batchTargets(targets);
    start(
        ReviewLibraryMetadataTaskResult::Kind::CoordinateBatchPreview,
        QStringLiteral("previewing"),
        [
            operations = operations_,
            batch,
            mode,
            latitude_degrees,
            longitude_degrees,
            place_name,
            source_label
        ]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind =
                ReviewLibraryMetadataTaskResult::Kind::CoordinateBatchPreview;
            result.coordinate_batch_preview = operations.preview_coordinates(
                batch,
                mode,
                latitude_degrees,
                longitude_degrees,
                place_name,
                source_label
            );
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::applyCoordinates(
    const QString& preview_id
) {
    start(
        ReviewLibraryMetadataTaskResult::Kind::CoordinateBatchApply,
        QStringLiteral("applying"),
        [operations = operations_, preview_id]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind =
                ReviewLibraryMetadataTaskResult::Kind::CoordinateBatchApply;
            result.batch_receipt = operations.apply_coordinates(preview_id);
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::previewGpx(
    const QString& gpx_path,
    const QVariantList& targets,
    const std::int64_t camera_clock_offset_seconds,
    const std::uint32_t maximum_gap_seconds
) {
    const QVector<BackendBatchPhotoTarget> batch = batchTargets(targets);
    start(
        ReviewLibraryMetadataTaskResult::Kind::GpxPreview,
        QStringLiteral("previewing"),
        [
            operations = operations_,
            gpx_path,
            batch,
            camera_clock_offset_seconds,
            maximum_gap_seconds
        ]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind = ReviewLibraryMetadataTaskResult::Kind::GpxPreview;
            result.gpx_preview = operations.preview_gpx(
                gpx_path,
                batch,
                camera_clock_offset_seconds,
                maximum_gap_seconds
            );
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::applyGpx(const QString& preview_id) {
    start(
        ReviewLibraryMetadataTaskResult::Kind::GpxApply,
        QStringLiteral("applying"),
        [operations = operations_, preview_id]() {
            ReviewLibraryMetadataTaskResult result;
            result.kind = ReviewLibraryMetadataTaskResult::Kind::GpxApply;
            result.batch_receipt = operations.apply_gpx(preview_id);
            return result;
        }
    );
}

void ReviewLibraryMetadataCoordinator::start(
    const ReviewLibraryMetadataTaskResult::Kind kind,
    QString status_code,
    std::function<ReviewLibraryMetadataTaskResult()> task
) {
    if (running_) {
        return;
    }
    running_ = true;
    error_.clear();
    status_code_ = std::move(status_code);
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run(
        [kind, task = std::move(task)]() mutable {
            try {
                return task();
            } catch (const std::exception& error) {
                ReviewLibraryMetadataTaskResult result;
                result.kind = kind;
                result.error = QString::fromUtf8(error.what());
                return result;
            }
        }
    ));
}

void ReviewLibraryMetadataCoordinator::finish() {
    ReviewLibraryMetadataTaskResult result = watcher_.result();
    running_ = false;
    if (!result.error.isEmpty()) {
        error_ = std::move(result.error);
        status_code_ = QStringLiteral("failed");
        emit stateChanged();
        return;
    }
    error_.clear();
    switch (result.kind) {
    case ReviewLibraryMetadataTaskResult::Kind::Load:
        metadata_ = std::move(result.metadata);
        status_code_ = QStringLiteral("idle");
        break;
    case ReviewLibraryMetadataTaskResult::Kind::CaptureMutation:
    case ReviewLibraryMetadataTaskResult::Kind::CoordinatesMutation:
        metadata_ = std::move(result.metadata);
        batch_receipt_ = {};
        status_code_ = QStringLiteral("saved");
        emit libraryChanged(metadata_.photo_id);
        break;
    case ReviewLibraryMetadataTaskResult::Kind::CaptureBatchPreview:
        capture_time_preview_ = std::move(result.capture_time_preview);
        batch_receipt_ = {};
        status_code_ = QStringLiteral("ready");
        break;
    case ReviewLibraryMetadataTaskResult::Kind::CaptureBatchApply:
        batch_receipt_ = result.batch_receipt;
        capture_time_preview_ = {};
        status_code_ = QStringLiteral("applied");
        emit libraryChanged({});
        break;
    case ReviewLibraryMetadataTaskResult::Kind::CoordinateBatchPreview:
        coordinate_batch_preview_ =
            std::move(result.coordinate_batch_preview);
        batch_receipt_ = {};
        status_code_ = QStringLiteral("ready");
        break;
    case ReviewLibraryMetadataTaskResult::Kind::CoordinateBatchApply:
        batch_receipt_ = result.batch_receipt;
        coordinate_batch_preview_ = {};
        status_code_ = QStringLiteral("applied");
        emit libraryChanged({});
        break;
    case ReviewLibraryMetadataTaskResult::Kind::GpxPreview:
        gpx_preview_ = std::move(result.gpx_preview);
        batch_receipt_ = {};
        status_code_ = QStringLiteral("ready");
        break;
    case ReviewLibraryMetadataTaskResult::Kind::GpxApply:
        batch_receipt_ = result.batch_receipt;
        gpx_preview_ = {};
        status_code_ = QStringLiteral("applied");
        emit libraryChanged({});
        break;
    }
    emit stateChanged();
}

QVector<BackendBatchPhotoTarget>
ReviewLibraryMetadataCoordinator::batchTargets(const QVariantList& targets) {
    QVector<BackendBatchPhotoTarget> batch;
    batch.reserve(targets.size());
    for (const QVariant& value : targets) {
        const QVariantMap target = value.toMap();
        const QString photo_id =
            target.value(QStringLiteral("photoId")).toString();
        if (photo_id.isEmpty()) {
            continue;
        }
        batch.push_back({
            .photo_id = photo_id,
            .source_path =
                target.value(QStringLiteral("sourcePath")).toString(),
        });
    }
    return batch;
}
