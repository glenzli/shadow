#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <cstddef>
#include <optional>

namespace shadow::desktop::maps {

struct GoogleMapTileId {
    int zoom = 0;
    int x = 0;
    int y = 0;

    friend bool
    operator==(const GoogleMapTileId& left, const GoogleMapTileId& right) noexcept = default;
};

[[nodiscard]] size_t qHash(const GoogleMapTileId& tile, size_t seed = 0) noexcept;

struct GoogleMapSession {
    QString token;
    QDateTime expires_at_utc;
    int tile_width = 256;
    int tile_height = 256;
    QString image_format;
};

struct GoogleMapViewportInfo {
    QString copyright_text;
    std::optional<int> maximum_zoom;
};

struct HttpCachePolicy {
    bool no_store = false;
    bool no_cache = false;
    bool must_revalidate = false;
    bool is_private = false;
    qint64 max_age_seconds = 0;
    qint64 stale_while_revalidate_seconds = 0;
    QByteArray etag;

    [[nodiscard]] bool freshAt(const QDateTime& stored_at_utc, const QDateTime& now_utc) const;
    [[nodiscard]] bool
    staleUsableAt(const QDateTime& stored_at_utc, const QDateTime& now_utc) const;
};

[[nodiscard]] QByteArray
makeSessionRequestBody(const QString& map_type, const QString& language, const QString& region);

[[nodiscard]] std::optional<GoogleMapSession>
parseSessionResponse(const QByteArray& payload, QString* error);

[[nodiscard]] std::optional<GoogleMapViewportInfo>
parseViewportResponse(const QByteArray& payload, QString* error);

[[nodiscard]] HttpCachePolicy
parseHttpCachePolicy(const QByteArray& cache_control, const QByteArray& etag);

/// Returns a stable, non-localized machine status. Raw provider text remains a
/// diagnostic and must not be projected directly into QML.
[[nodiscard]] QString parseGoogleMapErrorReason(const QByteArray& payload);

} // namespace shadow::desktop::maps
