#pragma once

#include "map/google_map_tiles_protocol.hpp"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>

class MapProviderPreferences;
class QNetworkAccessManager;
class QNetworkReply;

namespace shadow::desktop::maps {

/// Owns the complete Google Map Tiles network lifecycle.
///
/// The service is active only while an explicitly selected Google map is
/// visible. It keeps no disk cache, never prefetches, and exposes only decoded
/// display images to the native tile layer. The API key never crosses into QML.
class GoogleMapTilesService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusCode READ statusCode NOTIFY statusChanged)
    Q_PROPERTY(QString copyrightText READ copyrightText NOTIFY viewportInfoChanged)
    Q_PROPERTY(int tileWidth READ tileWidth NOTIFY sessionChanged)
    Q_PROPERTY(int tileHeight READ tileHeight NOTIFY sessionChanged)
    Q_PROPERTY(int maximumZoom READ maximumZoom NOTIFY viewportInfoChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString region READ region WRITE setRegion NOTIFY regionChanged)

  public:
    explicit GoogleMapTilesService(MapProviderPreferences* preferences, QObject* parent = nullptr);
    GoogleMapTilesService(
        MapProviderPreferences* preferences,
        QNetworkAccessManager* network,
        const QUrl& base_url,
        QObject* parent = nullptr
    );
    ~GoogleMapTilesService() override;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] QString copyrightText() const;
    [[nodiscard]] int tileWidth() const noexcept;
    [[nodiscard]] int tileHeight() const noexcept;
    [[nodiscard]] int maximumZoom() const noexcept;
    [[nodiscard]] QString language() const;
    [[nodiscard]] QString region() const;

    void setActive(bool active);
    void setLanguage(const QString& language);
    void setRegion(const QString& region);

    Q_INVOKABLE void updateViewport(double north, double south, double east, double west, int zoom);

    void setVisibleTiles(const QSet<GoogleMapTileId>& tiles);
    void requestTile(const GoogleMapTileId& tile);
    [[nodiscard]] QImage tileImage(const GoogleMapTileId& tile) const;

  signals:
    void activeChanged();
    void readyChanged();
    void busyChanged();
    void statusChanged();
    void sessionChanged();
    void viewportInfoChanged();
    void languageChanged();
    void regionChanged();
    void tileReady(int zoom, int x, int y);
    void tileUnavailable(int zoom, int x, int y);

  private:
    struct CachedTile {
        QImage image;
        HttpCachePolicy policy;
        QDateTime stored_at_utc;
        quint64 last_use = 0;
    };

    struct Viewport {
        double north = 0.0;
        double south = 0.0;
        double east = 0.0;
        double west = 0.0;
        int zoom = 0;
        bool valid = false;
    };

    void beginSession();
    void finishSession(QNetworkReply* reply, quint64 generation);
    void sendViewport();
    void finishViewport(QNetworkReply* reply, quint64 generation);
    void enqueueTile(const GoogleMapTileId& tile);
    void pumpTileQueue();
    void finishTile(QNetworkReply* reply, quint64 generation);
    void scheduleRetry(const GoogleMapTileId& tile);
    void clearRuntime();
    void abortRuntimeReplies();
    void evictCache();
    void refreshBusy();
    void setReady(bool ready);
    void setStatus(const QString& status);
    void synchronizeMapType();
    [[nodiscard]] QUrl endpoint(const QString& path) const;
    [[nodiscard]] bool sessionExpired() const;

    MapProviderPreferences* preferences_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QUrl base_url_;
    QString api_key_;
    QString map_type_;
    QString status_code_;
    QString language_ = QStringLiteral("en-US");
    QString region_ = QStringLiteral("US");
    GoogleMapSession session_;
    Viewport viewport_;
    QString copyright_text_;
    int maximum_zoom_ = 22;
    bool active_ = false;
    bool ready_ = false;
    bool busy_ = false;
    quint64 generation_ = 0;
    quint64 use_counter_ = 0;
    qsizetype cached_bytes_ = 0;
    int retry_delay_ms_ = 1000;

    QNetworkReply* session_reply_ = nullptr;
    QNetworkReply* viewport_reply_ = nullptr;
    QHash<QNetworkReply*, GoogleMapTileId> tile_replies_;
    QQueue<GoogleMapTileId> tile_queue_;
    QSet<GoogleMapTileId> queued_tiles_;
    QSet<GoogleMapTileId> visible_tiles_;
    QSet<GoogleMapTileId> retry_tiles_;
    QHash<GoogleMapTileId, int> retry_attempts_;
    QHash<GoogleMapTileId, CachedTile> cache_;
    QHash<GoogleMapTileId, QImage> visible_images_;
    QTimer retry_timer_;
};

} // namespace shadow::desktop::maps
