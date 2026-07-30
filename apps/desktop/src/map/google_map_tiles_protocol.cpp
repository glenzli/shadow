#include "map/google_map_tiles_protocol.hpp"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimeZone>

#include <algorithm>
#include <limits>

namespace shadow::desktop::maps {
namespace {

constexpr qint64 maximum_cache_seconds = 31LL * 24LL * 60LL * 60LL;

[[nodiscard]] qint64 boundedSeconds(const QByteArray& value) {
    QByteArray normalized = value.trimmed();
    if (normalized.size() >= 2 && normalized.front() == '"' && normalized.back() == '"') {
        normalized = normalized.mid(1, normalized.size() - 2);
    }
    bool parsed = false;
    const qint64 seconds = normalized.toLongLong(&parsed);
    if (!parsed || seconds <= 0) {
        return 0;
    }
    return std::min(seconds, maximum_cache_seconds);
}

[[nodiscard]] QString normalizedErrorReason(QString reason) {
    reason = reason.trimmed();
    if (reason.isEmpty()) {
        return QStringLiteral("provider-error");
    }
    static const QHash<QString, QString> reasons{
        {QStringLiteral("required"), QStringLiteral("required")},
        {QStringLiteral("notFound"), QStringLiteral("not-found")},
        {QStringLiteral("invalid"), QStringLiteral("invalid")},
        {QStringLiteral("forbidden"), QStringLiteral("forbidden")},
        {QStringLiteral("expired"), QStringLiteral("session-expired")},
        {QStringLiteral("badRequest"), QStringLiteral("bad-request")},
        {QStringLiteral("quotaExceeded"), QStringLiteral("quota-exceeded")},
        {QStringLiteral("rateLimitExceeded"), QStringLiteral("rate-limited")},
    };
    const auto match = reasons.constFind(reason);
    if (match != reasons.constEnd()) {
        return *match;
    }
    return QStringLiteral("provider-error");
}

} // namespace

size_t qHash(const GoogleMapTileId& tile, const size_t seed) noexcept {
    size_t result = ::qHash(tile.zoom, seed);
    result = ::qHash(tile.x, result);
    return ::qHash(tile.y, result);
}

bool HttpCachePolicy::freshAt(const QDateTime& stored_at_utc, const QDateTime& now_utc) const {
    if (no_store || no_cache || max_age_seconds <= 0 || !stored_at_utc.isValid()
        || !now_utc.isValid()) {
        return false;
    }
    return stored_at_utc.secsTo(now_utc) < max_age_seconds;
}

bool HttpCachePolicy::staleUsableAt(
    const QDateTime& stored_at_utc,
    const QDateTime& now_utc
) const {
    if (no_store || no_cache || must_revalidate || stale_while_revalidate_seconds <= 0
        || !stored_at_utc.isValid() || !now_utc.isValid()) {
        return false;
    }
    const qint64 age = stored_at_utc.secsTo(now_utc);
    return age >= max_age_seconds && age < max_age_seconds + stale_while_revalidate_seconds;
}

QByteArray
makeSessionRequestBody(const QString& map_type, const QString& language, const QString& region) {
    QJsonObject request{
        {QStringLiteral("mapType"), map_type},
        {QStringLiteral("language"), language},
        {QStringLiteral("region"), region},
    };
    if (map_type == QStringLiteral("terrain")) {
        request.insert(QStringLiteral("layerRoadmap"), true);
    }
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

std::optional<GoogleMapSession>
parseSessionResponse(const QByteArray& payload, QString* const error) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid-session-json");
        }
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QString token = object.value(QStringLiteral("session")).toString();
    const qint64 expiry = object.value(QStringLiteral("expiry")).toInteger();
    const int tile_width = object.value(QStringLiteral("tileWidth")).toInt();
    const int tile_height = object.value(QStringLiteral("tileHeight")).toInt();
    const QString image_format = object.value(QStringLiteral("imageFormat")).toString();
    if (token.isEmpty() || expiry <= 0 || tile_width <= 0 || tile_height <= 0 || tile_width > 2048
        || tile_height > 2048 || image_format.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid-session-contract");
        }
        return std::nullopt;
    }
    return GoogleMapSession{
        .token = token,
        .expires_at_utc = QDateTime::fromSecsSinceEpoch(expiry, QTimeZone::UTC),
        .tile_width = tile_width,
        .tile_height = tile_height,
        .image_format = image_format,
    };
}

std::optional<GoogleMapViewportInfo>
parseViewportResponse(const QByteArray& payload, QString* const error) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid-viewport-json");
        }
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    GoogleMapViewportInfo result{
        .copyright_text = object.value(QStringLiteral("copyright")).toString().trimmed(),
    };
    if (result.copyright_text.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid-viewport-contract");
        }
        return std::nullopt;
    }
    const QJsonArray rectangles = object.value(QStringLiteral("maxZoomRects")).toArray();
    for (const QJsonValue& value : rectangles) {
        const int maximum_zoom = value.toObject().value(QStringLiteral("maxZoom")).toInt(-1);
        if (maximum_zoom < 0) {
            continue;
        }
        result.maximum_zoom = result.maximum_zoom.has_value()
                                  ? std::min(*result.maximum_zoom, maximum_zoom)
                                  : maximum_zoom;
    }
    return result;
}

HttpCachePolicy parseHttpCachePolicy(const QByteArray& cache_control, const QByteArray& etag) {
    HttpCachePolicy result;
    result.etag = etag.trimmed();
    const QList<QByteArray> directives = cache_control.split(',');
    for (QByteArray directive : directives) {
        directive = directive.trimmed().toLower();
        const qsizetype separator = directive.indexOf('=');
        const QByteArray name = separator < 0 ? directive : directive.first(separator).trimmed();
        const QByteArray value = separator < 0 ? QByteArray{} : directive.sliced(separator + 1);
        if (name == "no-store") {
            result.no_store = true;
        } else if (name == "no-cache") {
            result.no_cache = true;
        } else if (name == "must-revalidate") {
            result.must_revalidate = true;
        } else if (name == "private") {
            result.is_private = true;
        } else if (name == "max-age") {
            result.max_age_seconds = boundedSeconds(value);
        } else if (name == "stale-while-revalidate") {
            result.stale_while_revalidate_seconds = boundedSeconds(value);
        }
    }
    if (result.no_store) {
        result.max_age_seconds = 0;
        result.stale_while_revalidate_seconds = 0;
        result.etag.clear();
    }
    return result;
}

QString parseGoogleMapErrorReason(const QByteArray& payload) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return QStringLiteral("provider-error");
    }
    const QJsonObject root = document.object();
    const QJsonObject error = root.value(QStringLiteral("error")).toObject();
    const QJsonArray details = error.value(QStringLiteral("details")).toArray();
    for (const QJsonValue& detail : details) {
        const QString reason = detail.toObject().value(QStringLiteral("reason")).toString();
        if (!reason.isEmpty()) {
            return normalizedErrorReason(reason);
        }
    }
    const QString direct_reason = error.value(QStringLiteral("reason"))
                                      .toString(root.value(QStringLiteral("reason")).toString());
    return normalizedErrorReason(direct_reason);
}

} // namespace shadow::desktop::maps
