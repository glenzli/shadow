#pragma once

#include <cstdint>
#include <span>

namespace shadow::image {

// Compact, bounds-checked EXIF subset shared by rendered raster providers. This is deliberately
// camera-neutral: it carries only the metadata Shadow can show and use for catalog filtering,
// never RAW sensor fields.
struct RasterExif final {
    std::uint16_t orientation = 1U;
    char make[128]{};
    char model[128]{};
    char lens_make[128]{};
    char lens_model[256]{};
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    double focal_length_35mm = 0.0;
    bool has_gps_coordinates = false;
    double gps_latitude_degrees = 0.0;
    double gps_longitude_degrees = 0.0;
    bool has_gps_altitude = false;
    double gps_altitude_meters = 0.0;
};

// Parses a TIFF header and the ordinary IFD0/ExifIFD fields from a bounded byte span. Invalid
// tags are ignored so a partially malformed optional EXIF block cannot make otherwise readable
// pixels unsafe; the caller still owns container-level corruption policy.
void parse_tiff_exif(std::span<const std::uint8_t> tiff, RasterExif& exif) noexcept;

} // namespace shadow::image
