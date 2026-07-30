#include "photo_inspection_projection.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <algorithm>
#include <limits>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

} // namespace

BackendPhotoInspection project_photo_inspection(
    const shadow::desktop::FfiPhotoInspection& source
) {
    return {
        .available = source.available,
        .photo_id = qstring(source.photo_id),
        .representation_id = qstring(source.representation_id),
        .source_path = qstring(source.source_path),
        .source_byte_len = source.source_byte_len,
        .has_source_modified_at = source.has_source_modified_at,
        .source_modified_at_ms = source.source_modified_at_ms,
        .has_metadata = source.has_metadata,
        .camera_make = qstring(source.camera_make),
        .camera_model = qstring(source.camera_model),
        .lens_make = qstring(source.lens_make),
        .lens_model = qstring(source.lens_model),
        .has_captured_at = source.has_captured_at,
        .captured_at_unix_seconds = source.captured_at_unix_seconds,
        .has_coordinates = source.has_coordinates,
        .latitude_e7 = source.latitude_e7,
        .longitude_e7 = source.longitude_e7,
        .place_name = qstring(source.place_name),
        .has_iso_speed = source.has_iso_speed,
        .iso_speed = source.iso_speed,
        .has_exposure_time = source.has_exposure_time,
        .exposure_time_seconds = source.exposure_time_seconds,
        .has_aperture = source.has_aperture,
        .aperture_f_number = source.aperture_f_number,
        .has_focal_length = source.has_focal_length,
        .focal_length_mm = source.focal_length_mm,
        .has_focal_length_35mm = source.has_focal_length_35mm,
        .focal_length_35mm = source.focal_length_35mm,
        .has_raw_dimensions = source.has_raw_dimensions,
        .raw_width = source.raw_width,
        .raw_height = source.raw_height,
        .has_sensor_bits = source.has_sensor_bits,
        .sensor_bits = source.sensor_bits,
        .cfa_pattern = qstring(source.cfa_pattern),
        .dng_version = qstring(source.dng_version),
        .has_technical_observation = source.has_technical_observation,
        .technical_input_width = source.technical_input_width,
        .technical_input_height = source.technical_input_height,
        .technical_preprocessing_version = qstring(
            source.technical_preprocessing_version
        ),
        .technical_implementation_version = qstring(
            source.technical_implementation_version
        ),
        .mean_luma = source.mean_luma,
        .p01_luma = source.p01_luma,
        .p50_luma = source.p50_luma,
        .p99_luma = source.p99_luma,
        .near_black_fraction = source.near_black_fraction,
        .near_white_fraction = source.near_white_fraction,
        .laplacian_variance = source.laplacian_variance,
        .edge_energy = source.edge_energy,
    };
}
