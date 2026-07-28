#include "desktop_backend.hpp"
#include "photo_inspection_projection.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>

#include <cmath>
#include <concepts>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

static_assert(std::same_as<
    decltype(std::declval<const DesktopBackend&>().photoInspection(
        QString{},
        QString{}
    )),
    BackendPhotoInspection
>);

namespace {

void require(const bool condition, const std::string& field) {
    if (!condition) {
        std::cerr << "photo inspection projection changed field: " << field << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void require_double(
    const double actual,
    const double expected,
    const std::string& field
) {
    require(std::abs(actual - expected) < 1.0e-12, field);
}

void complete_ffi_projection_preserves_every_field() {
    shadow::desktop::FfiPhotoInspection source;
    source.available = true;
    source.photo_id = rust::String("photo-identity");
    source.representation_id = rust::String("representation-identity");
    source.source_path = rust::String("/照片/source.nef");
    source.source_byte_len = 9'876'543;
    source.has_source_modified_at = false;
    source.source_modified_at_ms = 1'700'000'000'123;
    source.has_metadata = true;
    source.camera_make = rust::String("Nikon");
    source.camera_model = rust::String("Z 9");
    source.lens_make = rust::String("Nikkor");
    source.lens_model = rust::String("Z 100-400mm");
    source.has_captured_at = false;
    source.captured_at_unix_seconds = 1'700'000'321;
    source.has_iso_speed = true;
    source.iso_speed = 64.5;
    source.has_exposure_time = false;
    source.exposure_time_seconds = 0.008;
    source.has_aperture = true;
    source.aperture_f_number = 6.3;
    source.has_focal_length = false;
    source.focal_length_mm = 240.25;
    source.has_focal_length_35mm = true;
    source.focal_length_35mm = 360.5;
    source.has_raw_dimensions = false;
    source.raw_width = 8'256;
    source.raw_height = 5'504;
    source.has_sensor_bits = true;
    source.sensor_bits = 14;
    source.cfa_pattern = rust::String("RGGB");
    source.dng_version = rust::String("1.6.0.0");
    source.has_technical_observation = true;
    source.technical_input_width = 2'048;
    source.technical_input_height = 1'365;
    source.technical_preprocessing_version = rust::String("display-linear-v7");
    source.technical_implementation_version = rust::String("analysis-v11");
    source.mean_luma = 0.11;
    source.p01_luma = 0.012;
    source.p50_luma = 0.42;
    source.p99_luma = 0.93;
    source.near_black_fraction = 0.021;
    source.near_white_fraction = 0.034;
    source.laplacian_variance = 18.75;
    source.edge_energy = 4.125;

    const BackendPhotoInspection result = project_photo_inspection(source);

    require(result.available, "available");
    require(result.photo_id == QStringLiteral("photo-identity"), "photo_id");
    require(
        result.representation_id == QStringLiteral("representation-identity"),
        "representation_id"
    );
    require(result.source_path == QString::fromUtf8("/照片/source.nef"), "source_path");
    require(result.source_byte_len == 9'876'543, "source_byte_len bytes");
    require(!result.has_source_modified_at, "has_source_modified_at false");
    require(
        result.source_modified_at_ms == 1'700'000'000'123,
        "source_modified_at_ms milliseconds"
    );
    require(result.has_metadata, "has_metadata");
    require(result.camera_make == QStringLiteral("Nikon"), "camera_make");
    require(result.camera_model == QStringLiteral("Z 9"), "camera_model");
    require(result.lens_make == QStringLiteral("Nikkor"), "lens_make");
    require(result.lens_model == QStringLiteral("Z 100-400mm"), "lens_model");
    require(!result.has_captured_at, "has_captured_at false");
    require(
        result.captured_at_unix_seconds == 1'700'000'321,
        "captured_at_unix_seconds seconds"
    );
    require(result.has_iso_speed, "has_iso_speed");
    require_double(result.iso_speed, 64.5, "iso_speed");
    require(!result.has_exposure_time, "has_exposure_time false");
    require_double(
        result.exposure_time_seconds,
        0.008,
        "exposure_time_seconds seconds"
    );
    require(result.has_aperture, "has_aperture");
    require_double(result.aperture_f_number, 6.3, "aperture_f_number");
    require(!result.has_focal_length, "has_focal_length false");
    require_double(result.focal_length_mm, 240.25, "focal_length_mm");
    require(result.has_focal_length_35mm, "has_focal_length_35mm");
    require_double(
        result.focal_length_35mm,
        360.5,
        "focal_length_35mm millimeters"
    );
    require(!result.has_raw_dimensions, "has_raw_dimensions false");
    require(result.raw_width == 8'256, "raw_width pixels");
    require(result.raw_height == 5'504, "raw_height pixels");
    require(result.has_sensor_bits, "has_sensor_bits");
    require(result.sensor_bits == 14, "sensor_bits bits");
    require(result.cfa_pattern == QStringLiteral("RGGB"), "cfa_pattern");
    require(result.dng_version == QStringLiteral("1.6.0.0"), "dng_version");
    require(
        result.has_technical_observation,
        "has_technical_observation"
    );
    require(
        result.technical_input_width == 2'048,
        "technical_input_width pixels"
    );
    require(
        result.technical_input_height == 1'365,
        "technical_input_height pixels"
    );
    require(
        result.technical_preprocessing_version
            == QStringLiteral("display-linear-v7"),
        "technical_preprocessing_version"
    );
    require(
        result.technical_implementation_version
            == QStringLiteral("analysis-v11"),
        "technical_implementation_version"
    );
    require_double(result.mean_luma, 0.11, "mean_luma");
    require_double(result.p01_luma, 0.012, "p01_luma");
    require_double(result.p50_luma, 0.42, "p50_luma");
    require_double(result.p99_luma, 0.93, "p99_luma");
    require_double(
        result.near_black_fraction,
        0.021,
        "near_black_fraction"
    );
    require_double(
        result.near_white_fraction,
        0.034,
        "near_white_fraction"
    );
    require_double(
        result.laplacian_variance,
        18.75,
        "laplacian_variance"
    );
    require_double(result.edge_energy, 4.125, "edge_energy");
}

void exact_absent_identity_survives_the_real_backend_path() {
    QTemporaryDir root;
    require(root.isValid(), "temporary catalog root");

    const QString photo_id =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString representation_id =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const DesktopBackend backend(
        root.filePath(QStringLiteral("catalog.sqlite")),
        root.filePath(QStringLiteral("cache"))
    );
    const BackendPhotoInspection inspection =
        backend.photoInspection(photo_id, representation_id);
    require(!inspection.available, "absent available");
    require(inspection.photo_id == photo_id, "absent photo_id");
    require(
        inspection.representation_id == representation_id,
        "absent representation_id"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        complete_ffi_projection_preserves_every_field();
        exact_absent_identity_survives_the_real_backend_path();
    } catch (const std::exception& error) {
        std::cerr << "photo inspection production path failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
