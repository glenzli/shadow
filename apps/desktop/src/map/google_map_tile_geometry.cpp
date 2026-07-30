#include "map/google_map_tile_geometry.hpp"

#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace shadow::desktop::maps {
namespace {

constexpr double maximum_mercator_latitude = 85.05112878;
constexpr qsizetype maximum_visible_tiles = 512;

[[nodiscard]] double normalizedLongitude(double longitude) {
    double normalized = std::fmod(longitude + 180.0, 360.0);
    if (normalized < 0.0) {
        normalized += 360.0;
    }
    return normalized - 180.0;
}

[[nodiscard]] int wrappedTileX(const qint64 x, const int span) {
    const qint64 remainder = x % span;
    return static_cast<int>(remainder < 0 ? remainder + span : remainder);
}

} // namespace

GoogleMapTileLayout layoutVisibleGoogleMapTiles(
    const qreal viewport_width,
    const qreal viewport_height,
    const double center_latitude,
    const double center_longitude,
    const double zoom_level,
    const int tile_width,
    const int tile_height,
    const int maximum_zoom
) {
    GoogleMapTileLayout result;
    if (!std::isfinite(viewport_width) || !std::isfinite(viewport_height) || viewport_width <= 0.0
        || viewport_height <= 0.0 || !std::isfinite(center_latitude)
        || !std::isfinite(center_longitude) || !std::isfinite(zoom_level) || tile_width <= 0
        || tile_height <= 0) {
        return result;
    }

    result.request_zoom =
        std::clamp(static_cast<int>(std::floor(zoom_level)), 0, std::max(0, maximum_zoom));
    result.scale = std::exp2(zoom_level - result.request_zoom);
    const int tile_span = 1 << std::min(result.request_zoom, 30);
    const double world_width = static_cast<double>(tile_width) * static_cast<double>(tile_span);
    const double world_height = static_cast<double>(tile_height) * static_cast<double>(tile_span);
    const double longitude = normalizedLongitude(center_longitude);
    const double latitude =
        std::clamp(center_latitude, -maximum_mercator_latitude, maximum_mercator_latitude);
    const double latitude_radians = qDegreesToRadians(latitude);
    const double center_x = (longitude + 180.0) / 360.0 * world_width;
    const double center_y =
        (1.0 - std::asinh(std::tan(latitude_radians)) / std::numbers::pi) * 0.5 * world_height;
    const double half_width = static_cast<double>(viewport_width) / (2.0 * result.scale);
    const double half_height = static_cast<double>(viewport_height) / (2.0 * result.scale);
    const qint64 first_x = static_cast<qint64>(std::floor((center_x - half_width) / tile_width));
    const qint64 last_x = static_cast<qint64>(std::floor(
        std::nextafter(center_x + half_width, -std::numeric_limits<double>::infinity()) / tile_width
    ));
    const qint64 first_y_unclamped =
        static_cast<qint64>(std::floor((center_y - half_height) / tile_height));
    const qint64 last_y_unclamped = static_cast<qint64>(std::floor(
        std::nextafter(center_y + half_height, -std::numeric_limits<double>::infinity())
        / tile_height
    ));
    const qint64 first_y = std::clamp<qint64>(first_y_unclamped, 0, tile_span - 1);
    const qint64 last_y = std::clamp<qint64>(last_y_unclamped, 0, tile_span - 1);
    if (first_y > last_y) {
        return result;
    }

    for (qint64 y = first_y; y <= last_y; ++y) {
        for (qint64 x = first_x; x <= last_x; ++x) {
            if (result.tiles.size() >= maximum_visible_tiles) {
                return result;
            }
            const qreal destination_x = static_cast<qreal>(
                (static_cast<double>(x * tile_width) - center_x) * result.scale
                + static_cast<double>(viewport_width) / 2.0
            );
            const qreal destination_y = static_cast<qreal>(
                (static_cast<double>(y * tile_height) - center_y) * result.scale
                + static_cast<double>(viewport_height) / 2.0
            );
            result.tiles.push_back(
                GoogleMapVisibleTile{
                    .id =
                        {
                            .zoom = result.request_zoom,
                            .x = wrappedTileX(x, tile_span),
                            .y = static_cast<int>(y),
                        },
                    .unwrapped_x = x,
                    .destination = QRectF(
                        destination_x,
                        destination_y,
                        static_cast<qreal>(tile_width) * result.scale,
                        static_cast<qreal>(tile_height) * result.scale
                    ),
                }
            );
        }
    }
    return result;
}

} // namespace shadow::desktop::maps
