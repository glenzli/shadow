#include "amap_coordinate_transform.hpp"

#include <cmath>

namespace shadow::desktop::maps {
namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double SEMI_MAJOR_AXIS = 6378245.0;
constexpr double ECCENTRICITY_SQUARED = 0.00669342162296594323;

[[nodiscard]] double latitudeOffset(const double longitude, const double latitude) noexcept {
    double result = -100.0 + 2.0 * longitude + 3.0 * latitude
                    + 0.2 * latitude * latitude + 0.1 * longitude * latitude
                    + 0.2 * std::sqrt(std::abs(longitude));
    result += (20.0 * std::sin(6.0 * longitude * PI)
               + 20.0 * std::sin(2.0 * longitude * PI))
              * 2.0 / 3.0;
    result += (20.0 * std::sin(latitude * PI)
               + 40.0 * std::sin(latitude / 3.0 * PI))
              * 2.0 / 3.0;
    result += (160.0 * std::sin(latitude / 12.0 * PI)
               + 320.0 * std::sin(latitude * PI / 30.0))
              * 2.0 / 3.0;
    return result;
}

[[nodiscard]] double longitudeOffset(const double longitude, const double latitude) noexcept {
    double result = 300.0 + longitude + 2.0 * latitude + 0.1 * longitude * longitude
                    + 0.1 * longitude * latitude + 0.1 * std::sqrt(std::abs(longitude));
    result += (20.0 * std::sin(6.0 * longitude * PI)
               + 20.0 * std::sin(2.0 * longitude * PI))
              * 2.0 / 3.0;
    result += (20.0 * std::sin(longitude * PI)
               + 40.0 * std::sin(longitude / 3.0 * PI))
              * 2.0 / 3.0;
    result += (150.0 * std::sin(longitude / 12.0 * PI)
               + 300.0 * std::sin(longitude / 30.0 * PI))
              * 2.0 / 3.0;
    return result;
}

} // namespace

bool amapDomesticCoordinateSupported(const double latitude, const double longitude) noexcept {
    return std::isfinite(latitude) && std::isfinite(longitude) && longitude >= 72.004
           && longitude <= 137.8347 && latitude >= 0.8293 && latitude <= 55.8271;
}

GeographicCoordinate wgs84ToGcj02(const double latitude, const double longitude) noexcept {
    if (!amapDomesticCoordinateSupported(latitude, longitude)) {
        return {.latitude = latitude, .longitude = longitude};
    }
    double delta_latitude = latitudeOffset(longitude - 105.0, latitude - 35.0);
    double delta_longitude = longitudeOffset(longitude - 105.0, latitude - 35.0);
    const double radians = latitude / 180.0 * PI;
    const double sine = std::sin(radians);
    double magic = 1.0 - ECCENTRICITY_SQUARED * sine * sine;
    const double square_root = std::sqrt(magic);
    delta_latitude =
        (delta_latitude * 180.0)
        / ((SEMI_MAJOR_AXIS * (1.0 - ECCENTRICITY_SQUARED)) / (magic * square_root) * PI);
    delta_longitude =
        (delta_longitude * 180.0) / (SEMI_MAJOR_AXIS / square_root * std::cos(radians) * PI);
    return {
        .latitude = latitude + delta_latitude,
        .longitude = longitude + delta_longitude,
    };
}

GeographicCoordinate gcj02ToWgs84(const double latitude, const double longitude) noexcept {
    if (!amapDomesticCoordinateSupported(latitude, longitude)) {
        return {.latitude = latitude, .longitude = longitude};
    }
    GeographicCoordinate estimate{.latitude = latitude, .longitude = longitude};
    // Fixed-point refinement avoids persisting the provider offset while
    // keeping the conversion deterministic and well below photo-GPS accuracy.
    for (int iteration = 0; iteration < 4; ++iteration) {
        const GeographicCoordinate transformed =
            wgs84ToGcj02(estimate.latitude, estimate.longitude);
        estimate.latitude -= transformed.latitude - latitude;
        estimate.longitude -= transformed.longitude - longitude;
    }
    return estimate;
}

} // namespace shadow::desktop::maps
