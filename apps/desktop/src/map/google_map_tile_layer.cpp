#include "map/google_map_tile_layer.hpp"

#include "map/google_map_tiles_service.hpp"

#include <QPainter>

#include <cmath>

namespace shadow::desktop::maps {

GoogleMapTileLayer::GoogleMapTileLayer(QQuickItem* const parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setMipmap(false);
    setOpaquePainting(false);
}

QObject* GoogleMapTileLayer::service() const noexcept {
    return service_;
}

void GoogleMapTileLayer::setService(QObject* const service) {
    auto* const typed_service = qobject_cast<GoogleMapTilesService*>(service);
    if (service_ == typed_service) {
        return;
    }
    disconnectService();
    service_ = typed_service;
    if (service_ != nullptr) {
        service_connections_.push_back(connect(
            service_,
            &GoogleMapTilesService::tileReady,
            this,
            [this](const int zoom, const int x, const int y) {
                const GoogleMapTileId tile{.zoom = zoom, .x = x, .y = y};
                for (const GoogleMapVisibleTile& visible : layout_.tiles) {
                    if (visible.id == tile) {
                        update();
                        break;
                    }
                }
            }
        ));
        service_connections_.push_back(connect(
            service_,
            &GoogleMapTilesService::readyChanged,
            this,
            &GoogleMapTileLayer::refreshVisibleTiles
        ));
        service_connections_.push_back(connect(
            service_,
            &GoogleMapTilesService::sessionChanged,
            this,
            &GoogleMapTileLayer::refreshVisibleTiles
        ));
        service_connections_.push_back(connect(
            service_,
            &GoogleMapTilesService::viewportInfoChanged,
            this,
            &GoogleMapTileLayer::refreshVisibleTiles
        ));
        service_connections_.push_back(connect(service_, &QObject::destroyed, this, [this] {
            service_ = nullptr;
            layout_ = {};
            update();
            emit serviceChanged();
        }));
    }
    refreshVisibleTiles();
    emit serviceChanged();
}

double GoogleMapTileLayer::centerLatitude() const noexcept {
    return center_latitude_;
}

void GoogleMapTileLayer::setCenterLatitude(const double latitude) {
    if (!std::isfinite(latitude) || qFuzzyCompare(center_latitude_, latitude)) {
        return;
    }
    center_latitude_ = latitude;
    refreshVisibleTiles();
    emit centerLatitudeChanged();
}

double GoogleMapTileLayer::centerLongitude() const noexcept {
    return center_longitude_;
}

void GoogleMapTileLayer::setCenterLongitude(const double longitude) {
    if (!std::isfinite(longitude) || qFuzzyCompare(center_longitude_, longitude)) {
        return;
    }
    center_longitude_ = longitude;
    refreshVisibleTiles();
    emit centerLongitudeChanged();
}

double GoogleMapTileLayer::zoomLevel() const noexcept {
    return zoom_level_;
}

void GoogleMapTileLayer::setZoomLevel(const double zoom_level) {
    if (!std::isfinite(zoom_level) || qFuzzyCompare(zoom_level_, zoom_level)) {
        return;
    }
    zoom_level_ = zoom_level;
    refreshVisibleTiles();
    emit zoomLevelChanged();
}

bool GoogleMapTileLayer::active() const noexcept {
    return active_;
}

void GoogleMapTileLayer::setActive(const bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    refreshVisibleTiles();
    emit activeChanged();
}

void GoogleMapTileLayer::paint(QPainter* const painter) {
    if (!active_ || service_ == nullptr) {
        return;
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const GoogleMapVisibleTile& tile : layout_.tiles) {
        const QImage image = service_->tileImage(tile.id);
        if (!image.isNull()) {
            painter->drawImage(tile.destination, image);
        }
    }
}

void GoogleMapTileLayer::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickPaintedItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        refreshVisibleTiles();
    }
}

void GoogleMapTileLayer::refreshVisibleTiles() {
    if (!active_ || service_ == nullptr || width() <= 0.0 || height() <= 0.0) {
        layout_ = {};
        if (service_ != nullptr) {
            service_->setVisibleTiles({});
        }
        update();
        return;
    }
    layout_ = layoutVisibleGoogleMapTiles(
        width(),
        height(),
        center_latitude_,
        center_longitude_,
        zoom_level_,
        service_->tileWidth(),
        service_->tileHeight(),
        service_->maximumZoom()
    );
    QSet<GoogleMapTileId> visible_tiles;
    visible_tiles.reserve(layout_.tiles.size());
    for (const GoogleMapVisibleTile& tile : layout_.tiles) {
        visible_tiles.insert(tile.id);
    }
    service_->setVisibleTiles(visible_tiles);
    for (const GoogleMapVisibleTile& tile : layout_.tiles) {
        service_->requestTile(tile.id);
    }
    update();
}

void GoogleMapTileLayer::disconnectService() {
    for (const QMetaObject::Connection& connection : service_connections_) {
        disconnect(connection);
    }
    service_connections_.clear();
    if (service_ != nullptr) {
        service_->setVisibleTiles({});
    }
}

} // namespace shadow::desktop::maps
