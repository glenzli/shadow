#include "review_photo_inspection_coordinator.hpp"

#include <QDebug>
#include <QtConcurrentRun>

#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] ReviewPhotoInspectionTaskResult run_photo_inspection(
    const ReviewPhotoInspectionCoordinator::Loader& loader,
    ReviewPhotoInspectionRequest request
) {
    ReviewPhotoInspectionTaskResult result;
    result.request = std::move(request);
    try {
        result.inspection = loader(result.request.photo_id, result.request.representation_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

ReviewPhotoInspectionCoordinator::ReviewPhotoInspectionCoordinator(
    std::shared_ptr<DesktopBackend> backend,
    QObject* parent
) :
    ReviewPhotoInspectionCoordinator(
        [backend = std::move(backend)](const QString& photo_id, const QString& representation_id) {
            return backend->photoInspection(photo_id, representation_id);
        },
        parent
    ) {}

ReviewPhotoInspectionCoordinator::ReviewPhotoInspectionCoordinator(Loader loader, QObject* parent) :
    QObject(parent), loader_(std::move(loader)) {
    if (!loader_) {
        throw std::invalid_argument("photo inspection loader is required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<ReviewPhotoInspectionTaskResult>::finished,
        this,
        &ReviewPhotoInspectionCoordinator::finish
    );
}

ReviewPhotoInspectionCoordinator::~ReviewPhotoInspectionCoordinator() {
    watcher_.waitForFinished();
}

QVariantMap ReviewPhotoInspectionCoordinator::presentation() const {
    const auto& source = inspection_;
    if (source.photo_id.isEmpty() || source.representation_id.isEmpty()) {
        return {};
    }
    return {
        {QStringLiteral("available"), source.available},
        {QStringLiteral("photoId"), source.photo_id},
        {QStringLiteral("representationId"), source.representation_id},
        {QStringLiteral("sourcePath"), source.source_path},
        {
            QStringLiteral("sourceByteLen"),
            QVariant::fromValue(static_cast<qulonglong>(source.source_byte_len)),
        },
        {
            QStringLiteral("hasSourceModifiedAt"),
            source.has_source_modified_at,
        },
        {
            QStringLiteral("sourceModifiedAtMs"),
            source.source_modified_at_ms,
        },
        {QStringLiteral("hasMetadata"), source.has_metadata},
        {QStringLiteral("cameraMake"), source.camera_make},
        {QStringLiteral("cameraModel"), source.camera_model},
        {QStringLiteral("lensMake"), source.lens_make},
        {QStringLiteral("lensModel"), source.lens_model},
        {QStringLiteral("hasCapturedAt"), source.has_captured_at},
        {
            QStringLiteral("capturedAtUnixSeconds"),
            source.captured_at_unix_seconds,
        },
        {QStringLiteral("hasCoordinates"), source.has_coordinates},
        {
            QStringLiteral("latitude"),
            static_cast<double>(source.latitude_e7) / 10'000'000.0,
        },
        {
            QStringLiteral("longitude"),
            static_cast<double>(source.longitude_e7) / 10'000'000.0,
        },
        {QStringLiteral("placeName"), source.place_name},
        {QStringLiteral("resolvedPlaceName"), source.resolved_place_name},
        {QStringLiteral("hasIsoSpeed"), source.has_iso_speed},
        {QStringLiteral("isoSpeed"), source.iso_speed},
        {QStringLiteral("hasExposureTime"), source.has_exposure_time},
        {
            QStringLiteral("exposureTimeSeconds"),
            source.exposure_time_seconds,
        },
        {QStringLiteral("hasAperture"), source.has_aperture},
        {QStringLiteral("apertureFNumber"), source.aperture_f_number},
        {QStringLiteral("hasFocalLength"), source.has_focal_length},
        {QStringLiteral("focalLengthMm"), source.focal_length_mm},
        {
            QStringLiteral("hasFocalLength35mm"),
            source.has_focal_length_35mm,
        },
        {QStringLiteral("focalLength35mm"), source.focal_length_35mm},
        {QStringLiteral("hasRawDimensions"), source.has_raw_dimensions},
        {QStringLiteral("rawWidth"), source.raw_width},
        {QStringLiteral("rawHeight"), source.raw_height},
        {QStringLiteral("hasSensorBits"), source.has_sensor_bits},
        {QStringLiteral("sensorBits"), source.sensor_bits},
        {QStringLiteral("cfaPattern"), source.cfa_pattern},
        {QStringLiteral("dngVersion"), source.dng_version},
        {
            QStringLiteral("hasFocusObservation"),
            source.has_focus_observation,
        },
        {
            QStringLiteral("focusObservationSchemaVersion"),
            source.focus_observation_schema_version,
        },
        {
            QStringLiteral("focusObservationSource"),
            source.focus_observation_source,
        },
        {
            QStringLiteral("focusObservationCenterX"),
            source.focus_observation_center_x,
        },
        {
            QStringLiteral("focusObservationCenterY"),
            source.focus_observation_center_y,
        },
        {
            QStringLiteral("focusObservationWidth"),
            source.focus_observation_width,
        },
        {
            QStringLiteral("focusObservationHeight"),
            source.focus_observation_height,
        },
        {
            QStringLiteral("focusObservationConfirmed"),
            source.focus_observation_confirmed,
        },
        {
            QStringLiteral("focusObservationConfidence"),
            source.focus_observation_confidence,
        },
        {
            QStringLiteral("hasTechnicalObservation"),
            source.has_technical_observation,
        },
        {
            QStringLiteral("technicalInputWidth"),
            source.technical_input_width,
        },
        {
            QStringLiteral("technicalInputHeight"),
            source.technical_input_height,
        },
        {
            QStringLiteral("technicalPreprocessingVersion"),
            source.technical_preprocessing_version,
        },
        {
            QStringLiteral("technicalImplementationVersion"),
            source.technical_implementation_version,
        },
        {QStringLiteral("meanLuma"), source.mean_luma},
        {QStringLiteral("p01Luma"), source.p01_luma},
        {QStringLiteral("p50Luma"), source.p50_luma},
        {QStringLiteral("p99Luma"), source.p99_luma},
        {QStringLiteral("nearBlackFraction"), source.near_black_fraction},
        {QStringLiteral("nearWhiteFraction"), source.near_white_fraction},
        {QStringLiteral("laplacianVariance"), source.laplacian_variance},
        {QStringLiteral("edgeEnergy"), source.edge_energy},
    };
}

bool ReviewPhotoInspectionCoordinator::busy() const noexcept {
    return session_.current().valid() && (running_ || pending_);
}

bool ReviewPhotoInspectionCoordinator::failed() const noexcept {
    return !error_.isEmpty();
}

void ReviewPhotoInspectionCoordinator::request(
    const QString& photo_id,
    const QString& representation_id
) {
    if (photo_id.isEmpty() || representation_id.isEmpty()) {
        clear();
        return;
    }
    static_cast<void>(session_.request(photo_id, representation_id));
    inspection_ = {};
    error_.clear();
    if (running_) {
        pending_ = true;
    } else {
        start();
    }
    emit stateChanged();
}

void ReviewPhotoInspectionCoordinator::retry() {
    const ReviewPhotoInspectionRequest current = session_.current();
    if (!current.valid()) {
        return;
    }
    static_cast<void>(session_.request(current.photo_id, current.representation_id));
    error_.clear();
    if (running_) {
        pending_ = true;
    } else {
        start();
    }
    emit stateChanged();
}

void ReviewPhotoInspectionCoordinator::clear() {
    session_.clear();
    pending_ = false;
    inspection_ = {};
    error_.clear();
    emit stateChanged();
}

void ReviewPhotoInspectionCoordinator::start() {
    if (running_) {
        pending_ = true;
        return;
    }
    const ReviewPhotoInspectionRequest request = session_.current();
    pending_ = false;
    if (!request.valid()) {
        return;
    }
    running_ = true;
    watcher_.setFuture(QtConcurrent::run(run_photo_inspection, loader_, request));
}

void ReviewPhotoInspectionCoordinator::finish() {
    ReviewPhotoInspectionTaskResult result = watcher_.result();
    running_ = false;
    if (session_.accepts(result.request)) {
        if (!result.error.isEmpty()) {
            qWarning().noquote() << "Selected photo inspection failed for"
                                 << result.request.photo_id << result.request.representation_id
                                 << result.error;
            error_ = std::move(result.error);
        } else if (result.inspection.photo_id != result.request.photo_id
                   || result.inspection.representation_id != result.request.representation_id) {
            qWarning().noquote() << "Selected photo inspection returned a different identity for"
                                 << result.request.photo_id << result.request.representation_id;
            error_ = QStringLiteral("photo inspection returned a different identity");
        } else {
            inspection_ = std::move(result.inspection);
            error_.clear();
        }
    }
    if (pending_) {
        start();
    }
    emit stateChanged();
}
