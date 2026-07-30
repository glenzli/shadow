#pragma once

#include "map/google_map_tile_geometry.hpp"

#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace shadow::desktop::maps {

class GoogleMapTilesService;

/// Paints only the currently visible Google raster tiles. Gesture handling and
/// photo markers remain owned by the synchronized Qt Location overlay map.
class GoogleMapTileLayer : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(
        double centerLatitude READ centerLatitude WRITE setCenterLatitude NOTIFY
            centerLatitudeChanged
    )
    Q_PROPERTY(
        double centerLongitude READ centerLongitude WRITE setCenterLongitude NOTIFY
            centerLongitudeChanged
    )
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

  public:
    explicit GoogleMapTileLayer(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* service() const noexcept;
    void setService(QObject* service);
    [[nodiscard]] double centerLatitude() const noexcept;
    void setCenterLatitude(double latitude);
    [[nodiscard]] double centerLongitude() const noexcept;
    void setCenterLongitude(double longitude);
    [[nodiscard]] double zoomLevel() const noexcept;
    void setZoomLevel(double zoom_level);
    [[nodiscard]] bool active() const noexcept;
    void setActive(bool active);

    void paint(QPainter* painter) override;

  signals:
    void serviceChanged();
    void centerLatitudeChanged();
    void centerLongitudeChanged();
    void zoomLevelChanged();
    void activeChanged();

  protected:
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

  private:
    void refreshVisibleTiles();
    void disconnectService();

    QPointer<GoogleMapTilesService> service_;
    GoogleMapTileLayout layout_;
    QList<QMetaObject::Connection> service_connections_;
    double center_latitude_ = 20.0;
    double center_longitude_ = 0.0;
    double zoom_level_ = 2.5;
    bool active_ = false;
};

} // namespace shadow::desktop::maps
