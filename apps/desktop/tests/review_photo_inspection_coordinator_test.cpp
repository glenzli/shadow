#include "review_photo_inspection_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr
            << "photo inspection coordinator contract failed: "
            << message
            << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate>
void wait_until(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

[[nodiscard]] BackendPhotoInspection inspection_for(
    const QString& photo_id,
    const QString& representation_id
) {
    return {
        .available = true,
        .photo_id = photo_id,
        .representation_id = representation_id,
        .source_path = QStringLiteral("/source/photo.nef"),
        .source_byte_len = 512,
        .has_metadata = true,
        .camera_make = QStringLiteral("Nikon"),
    };
}

void complete_presentation_preserves_every_backend_field() {
    BackendPhotoInspection source;
    source.available = true;
    source.photo_id = QStringLiteral("photo-presentation");
    source.representation_id = QStringLiteral("representation-presentation");
    source.source_path = QString::fromUtf8("/照片/presentation.nef");
    source.source_byte_len = 7'654'321;
    source.has_source_modified_at = false;
    source.source_modified_at_ms = 1'700'000'000'987;
    source.has_metadata = true;
    source.camera_make = QStringLiteral("Nikon");
    source.camera_model = QStringLiteral("Z 8");
    source.lens_make = QStringLiteral("Nikkor");
    source.lens_model = QStringLiteral("Z 24-120mm");
    source.has_captured_at = false;
    source.captured_at_unix_seconds = 1'700'000'765;
    source.has_iso_speed = true;
    source.iso_speed = 125.5;
    source.has_exposure_time = false;
    source.exposure_time_seconds = 0.004;
    source.has_aperture = true;
    source.aperture_f_number = 5.6;
    source.has_focal_length = false;
    source.focal_length_mm = 86.25;
    source.has_focal_length_35mm = true;
    source.focal_length_35mm = 129.375;
    source.has_raw_dimensions = false;
    source.raw_width = 8'256;
    source.raw_height = 5'504;
    source.has_sensor_bits = true;
    source.sensor_bits = 14;
    source.cfa_pattern = QStringLiteral("RGGB");
    source.dng_version = QStringLiteral("1.6.0.0");
    source.has_focus_observation = true;
    source.focus_observation_schema_version = 1;
    source.focus_observation_source = QStringLiteral("camera_focus_area");
    source.focus_observation_center_x = 0.625;
    source.focus_observation_center_y = 0.375;
    source.focus_observation_width = 0.125;
    source.focus_observation_height = 0.25;
    source.focus_observation_confirmed = true;
    source.focus_observation_confidence = 1.0;
    source.has_technical_observation = true;
    source.technical_input_width = 2'048;
    source.technical_input_height = 1'365;
    source.technical_preprocessing_version = QStringLiteral("linear-v13");
    source.technical_implementation_version = QStringLiteral("analysis-v17");
    source.mean_luma = 0.17;
    source.p01_luma = 0.019;
    source.p50_luma = 0.47;
    source.p99_luma = 0.97;
    source.near_black_fraction = 0.023;
    source.near_white_fraction = 0.037;
    source.laplacian_variance = 22.75;
    source.edge_energy = 5.625;

    ReviewPhotoInspectionCoordinator coordinator(
        [source](const QString&, const QString&) { return source; }
    );
    coordinator.request(source.photo_id, source.representation_id);
    wait_until(
        [&]() { return !coordinator.busy(); },
        "complete presentation did not reach terminal state"
    );
    require(!coordinator.failed(), "complete presentation failed");
    const QVariantMap result = coordinator.presentation();
    require(result.value(QStringLiteral("available")).toBool(), "available");
    require(
        result.value(QStringLiteral("photoId")).toString() == source.photo_id,
        "photoId"
    );
    require(
        result.value(QStringLiteral("representationId")).toString()
            == source.representation_id,
        "representationId"
    );
    require(
        result.value(QStringLiteral("sourcePath")).toString() == source.source_path,
        "sourcePath UTF-8"
    );
    require(
        result.value(QStringLiteral("sourceByteLen")).toULongLong()
            == source.source_byte_len,
        "sourceByteLen bytes"
    );
    require(
        !result.value(QStringLiteral("hasSourceModifiedAt")).toBool(),
        "hasSourceModifiedAt false"
    );
    require(
        result.value(QStringLiteral("sourceModifiedAtMs")).toLongLong()
            == source.source_modified_at_ms,
        "sourceModifiedAtMs milliseconds"
    );
    require(result.value(QStringLiteral("hasMetadata")).toBool(), "hasMetadata");
    require(
        result.value(QStringLiteral("cameraMake")).toString()
            == source.camera_make,
        "cameraMake"
    );
    require(
        result.value(QStringLiteral("cameraModel")).toString()
            == source.camera_model,
        "cameraModel"
    );
    require(
        result.value(QStringLiteral("lensMake")).toString() == source.lens_make,
        "lensMake"
    );
    require(
        result.value(QStringLiteral("lensModel")).toString() == source.lens_model,
        "lensModel"
    );
    require(
        !result.value(QStringLiteral("hasCapturedAt")).toBool(),
        "hasCapturedAt false"
    );
    require(
        result.value(QStringLiteral("capturedAtUnixSeconds")).toLongLong()
            == source.captured_at_unix_seconds,
        "capturedAtUnixSeconds seconds"
    );
    require(result.value(QStringLiteral("hasIsoSpeed")).toBool(), "hasIsoSpeed");
    require(
        std::abs(
            result.value(QStringLiteral("isoSpeed")).toDouble()
            - source.iso_speed
        ) < 1.0e-12,
        "isoSpeed"
    );
    require(
        !result.value(QStringLiteral("hasExposureTime")).toBool(),
        "hasExposureTime false"
    );
    require(
        std::abs(
            result.value(QStringLiteral("exposureTimeSeconds")).toDouble()
            - source.exposure_time_seconds
        ) < 1.0e-12,
        "exposureTimeSeconds seconds"
    );
    require(result.value(QStringLiteral("hasAperture")).toBool(), "hasAperture");
    require(
        std::abs(
            result.value(QStringLiteral("apertureFNumber")).toDouble()
            - source.aperture_f_number
        ) < 1.0e-12,
        "apertureFNumber"
    );
    require(
        !result.value(QStringLiteral("hasFocalLength")).toBool(),
        "hasFocalLength false"
    );
    require(
        std::abs(
            result.value(QStringLiteral("focalLengthMm")).toDouble()
            - source.focal_length_mm
        ) < 1.0e-12,
        "focalLengthMm millimeters"
    );
    require(
        result.value(QStringLiteral("hasFocalLength35mm")).toBool(),
        "hasFocalLength35mm"
    );
    require(
        std::abs(
            result.value(QStringLiteral("focalLength35mm")).toDouble()
            - source.focal_length_35mm
        ) < 1.0e-12,
        "focalLength35mm millimeters"
    );
    require(
        !result.value(QStringLiteral("hasRawDimensions")).toBool(),
        "hasRawDimensions false"
    );
    require(
        result.value(QStringLiteral("rawWidth")).toUInt() == source.raw_width,
        "rawWidth pixels"
    );
    require(
        result.value(QStringLiteral("rawHeight")).toUInt() == source.raw_height,
        "rawHeight pixels"
    );
    require(
        result.value(QStringLiteral("hasSensorBits")).toBool(),
        "hasSensorBits"
    );
    require(
        result.value(QStringLiteral("sensorBits")).toUInt() == source.sensor_bits,
        "sensorBits bits"
    );
    require(
        result.value(QStringLiteral("cfaPattern")).toString()
            == source.cfa_pattern,
        "cfaPattern"
    );
    require(
        result.value(QStringLiteral("dngVersion")).toString()
            == source.dng_version,
        "dngVersion"
    );
    require(
        result.value(QStringLiteral("hasFocusObservation")).toBool(),
        "hasFocusObservation"
    );
    require(
        result.value(QStringLiteral("focusObservationSchemaVersion")).toUInt()
            == source.focus_observation_schema_version,
        "focusObservationSchemaVersion"
    );
    require(
        result.value(QStringLiteral("focusObservationSource")).toString()
            == source.focus_observation_source,
        "focusObservationSource"
    );
    require(
        std::abs(
            result.value(QStringLiteral("focusObservationCenterX")).toDouble()
            - source.focus_observation_center_x
        ) < 1.0e-12,
        "focusObservationCenterX"
    );
    require(
        std::abs(
            result.value(QStringLiteral("focusObservationCenterY")).toDouble()
            - source.focus_observation_center_y
        ) < 1.0e-12,
        "focusObservationCenterY"
    );
    require(
        result.value(QStringLiteral("focusObservationConfirmed")).toBool(),
        "focusObservationConfirmed"
    );
    require(
        result.value(QStringLiteral("hasTechnicalObservation")).toBool(),
        "hasTechnicalObservation"
    );
    require(
        result.value(QStringLiteral("technicalInputWidth")).toUInt()
            == source.technical_input_width,
        "technicalInputWidth pixels"
    );
    require(
        result.value(QStringLiteral("technicalInputHeight")).toUInt()
            == source.technical_input_height,
        "technicalInputHeight pixels"
    );
    require(
        result.value(QStringLiteral("technicalPreprocessingVersion")).toString()
            == source.technical_preprocessing_version,
        "technicalPreprocessingVersion"
    );
    require(
        result.value(QStringLiteral("technicalImplementationVersion")).toString()
            == source.technical_implementation_version,
        "technicalImplementationVersion"
    );
    const auto require_metric = [&](const char* key, const double expected) {
        require(
            std::abs(
                result.value(QString::fromLatin1(key)).toDouble() - expected
            ) < 1.0e-12,
            key
        );
    };
    require_metric("meanLuma", source.mean_luma);
    require_metric("p01Luma", source.p01_luma);
    require_metric("p50Luma", source.p50_luma);
    require_metric("p99Luma", source.p99_luma);
    require_metric("nearBlackFraction", source.near_black_fraction);
    require_metric("nearWhiteFraction", source.near_white_fraction);
    require_metric("laplacianVariance", source.laplacian_variance);
    require_metric("edgeEnergy", source.edge_energy);
}

void rapid_reselection_publishes_only_the_newest_identity() {
    QSemaphore first_started;
    QSemaphore release_first;
    ReviewPhotoInspectionCoordinator coordinator(
        [&](const QString& photo_id, const QString& representation_id) {
            if (photo_id == QStringLiteral("photo-a")) {
                first_started.release();
                release_first.acquire();
            }
            return inspection_for(photo_id, representation_id);
        }
    );

    coordinator.request(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );
    require(
        first_started.tryAcquire(1, 1'000),
        "first request did not enter the worker"
    );
    coordinator.request(
        QStringLiteral("photo-b"),
        QStringLiteral("representation-b")
    );
    release_first.release();

    wait_until(
        [&]() {
            return !coordinator.busy()
                && coordinator.presentation().value(
                    QStringLiteral("representationId")
                ).toString() == QStringLiteral("representation-b");
        },
        "rapid reselection did not settle on selection B"
    );
    require(!coordinator.failed(), "selection B unexpectedly failed");
}

void clear_rejects_an_in_flight_terminal_result() {
    QSemaphore started;
    QSemaphore release;
    int state_changes = 0;
    ReviewPhotoInspectionCoordinator coordinator(
        [&](const QString& photo_id, const QString& representation_id) {
            started.release();
            release.acquire();
            return inspection_for(photo_id, representation_id);
        }
    );
    QObject::connect(
        &coordinator,
        &ReviewPhotoInspectionCoordinator::stateChanged,
        [&]() { ++state_changes; }
    );

    coordinator.request(
        QStringLiteral("photo-clear"),
        QStringLiteral("representation-clear")
    );
    require(started.tryAcquire(1, 1'000), "clear request did not start");
    coordinator.clear();
    require(coordinator.presentation().isEmpty(), "clear retained presentation");
    require(!coordinator.busy(), "clear retained visible busy state");
    const int changes_before_completion = state_changes;
    release.release();

    wait_until(
        [&]() { return state_changes > changes_before_completion; },
        "cleared worker did not reach its terminal callback"
    );
    require(
        coordinator.presentation().isEmpty(),
        "cleared completion republished stale inspection"
    );
    require(!coordinator.failed(), "cleared completion published a failure");
}

void failure_is_terminal_and_explicit_retry_can_recover() {
    std::atomic<int> attempts = 0;
    ReviewPhotoInspectionCoordinator coordinator(
        [&](const QString& photo_id, const QString& representation_id) {
            if (++attempts == 1) {
                throw std::runtime_error("sentinel inspection failure");
            }
            return inspection_for(photo_id, representation_id);
        }
    );

    coordinator.request(
        QStringLiteral("photo-retry"),
        QStringLiteral("representation-retry")
    );
    wait_until(
        [&]() { return !coordinator.busy() && coordinator.failed(); },
        "worker error did not become a terminal failure"
    );
    require(
        coordinator.presentation().isEmpty(),
        "failed request exposed a no-metadata presentation"
    );

    coordinator.retry();
    wait_until(
        [&]() {
            return !coordinator.busy()
                && !coordinator.failed()
                && coordinator.presentation().value(
                    QStringLiteral("available")
                ).toBool();
        },
        "explicit retry did not recover the exact request"
    );
    require(attempts == 2, "retry issued an unexpected number of loads");
}

void identity_mismatch_is_a_terminal_failure() {
    ReviewPhotoInspectionCoordinator coordinator(
        [](const QString& photo_id, const QString&) {
            return inspection_for(
                photo_id,
                QStringLiteral("wrong-representation")
            );
        }
    );

    coordinator.request(
        QStringLiteral("photo-mismatch"),
        QStringLiteral("representation-mismatch")
    );
    wait_until(
        [&]() { return !coordinator.busy() && coordinator.failed(); },
        "identity mismatch did not become a terminal failure"
    );
    require(
        coordinator.presentation().isEmpty(),
        "identity mismatch published the wrong representation"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    complete_presentation_preserves_every_backend_field();
    rapid_reselection_publishes_only_the_newest_identity();
    clear_rejects_an_in_flight_terminal_result();
    failure_is_terminal_and_explicit_retry_can_recover();
    identity_mismatch_is_a_terminal_failure();
    return EXIT_SUCCESS;
}
