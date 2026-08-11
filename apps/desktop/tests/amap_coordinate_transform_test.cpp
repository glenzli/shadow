#include "amap_coordinate_transform.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "AMap coordinate transform contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool closeTo(const double left, const double right, const double tolerance) {
    return std::abs(left - right) <= tolerance;
}

} // namespace

int main() {
    using namespace shadow::desktop::maps;

    const GeographicCoordinate beijing = wgs84ToGcj02(39.908823, 116.397470);
    const GeographicCoordinate restored = gcj02ToWgs84(beijing.latitude, beijing.longitude);
    const GeographicCoordinate paris = wgs84ToGcj02(48.8566, 2.3522);

    return require(
               amapDomesticCoordinateSupported(39.908823, 116.397470)
                   && closeTo(beijing.latitude, 39.910226, 0.00002)
                   && closeTo(beijing.longitude, 116.403714, 0.00002),
               "WGS84 coordinates convert to the expected GCJ-02 provider coordinate"
           )
                   && require(
                       closeTo(restored.latitude, 39.908823, 0.0000002)
                           && closeTo(restored.longitude, 116.397470, 0.0000002),
                       "the provider coordinate converts back without changing Catalog identity"
                   )
                   && require(
                       !amapDomesticCoordinateSupported(48.8566, 2.3522)
                           && paris.latitude == 48.8566 && paris.longitude == 2.3522,
                       "coordinates outside the domestic service area remain WGS84"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
