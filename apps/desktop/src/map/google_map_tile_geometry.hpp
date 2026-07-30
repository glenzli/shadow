#pragma once

#include "map/google_map_tiles_protocol.hpp"

#include <QList>
#include <QRectF>

namespace shadow::desktop::maps {

struct GoogleMapVisibleTile {
    GoogleMapTileId id;
    qint64 unwrapped_x = 0;
    QRectF destination;
};

struct GoogleMapTileLayout {
    int request_zoom = 0;
    qreal scale = 1.0;
    QList<GoogleMapVisibleTile> tiles;
};

[[nodiscard]] GoogleMapTileLayout layoutVisibleGoogleMapTiles(
    qreal viewport_width,
    qreal viewport_height,
    double center_latitude,
    double center_longitude,
    double zoom_level,
    int tile_width,
    int tile_height,
    int maximum_zoom = 22
);

} // namespace shadow::desktop::maps
