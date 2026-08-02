#include "review_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

[[nodiscard]] std::int32_t
coordinate_e7(const double degrees, const double minimum, const double maximum) {
    const double bounded = std::clamp(degrees, minimum, maximum);
    return static_cast<std::int32_t>(std::llround(bounded * 10'000'000.0));
}

} // namespace

QVariantList ReviewController::libraryMapClusters() const {
    return map_coordinator_.clusters();
}

qulonglong ReviewController::libraryMapPhotoCount() const noexcept {
    return map_coordinator_.photoCount();
}

bool ReviewController::libraryMapBusy() const noexcept {
    return map_coordinator_.busy();
}

bool ReviewController::libraryMapFailed() const noexcept {
    return map_coordinator_.failed();
}

void ReviewController::requestLibraryMapViewport(
    const double south_latitude,
    const double west_longitude,
    const double north_latitude,
    const double east_longitude,
    const int columns,
    const int rows
) {
    if (!std::isfinite(south_latitude) || !std::isfinite(west_longitude)
        || !std::isfinite(north_latitude) || !std::isfinite(east_longitude)
        || south_latitude >= north_latitude || columns < 1 || rows < 1 || columns > 128
        || rows > 128 || columns * rows > 4'096) {
        return;
    }
    map_coordinator_.request(
        currentLibraryFilter(),
        BackendLibraryMapViewport{
            .south_latitude_e7 = coordinate_e7(south_latitude, -90.0, 90.0),
            .west_longitude_e7 = coordinate_e7(west_longitude, -180.0, 180.0),
            .north_latitude_e7 = coordinate_e7(north_latitude, -90.0, 90.0),
            .east_longitude_e7 = coordinate_e7(east_longitude, -180.0, 180.0),
        },
        BackendLibraryMapGrid{
            .columns = static_cast<std::uint16_t>(columns),
            .rows = static_cast<std::uint16_t>(rows),
        }
    );
}
