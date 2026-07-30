#include "map/google_map_tiles_service.hpp"

#include "map_provider_preferences.hpp"

#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace shadow::desktop::maps {
namespace {

constexpr qsizetype maximum_cached_bytes = 64 * 1024 * 1024;
constexpr qsizetype maximum_parallel_tile_requests = 8;
constexpr int maximum_retry_attempts = 5;
constexpr double maximum_viewport_longitude = 180.0 - 1e-7;
constexpr int maximum_google_2d_zoom = 22;

[[nodiscard]] QString normalizedLanguage(const QString& language) {
    const QString trimmed = language.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("en-US") : trimmed.left(32);
}

[[nodiscard]] QString normalizedRegion(const QString& region) {
    const QString trimmed = region.trimmed().toUpper();
    return trimmed.size() == 2 ? trimmed : QStringLiteral("US");
}

[[nodiscard]] QString statusForHttpCode(const int status_code) {
    if (status_code == 401 || status_code == 403) {
        return QStringLiteral("forbidden");
    }
    if (status_code == 404) {
        return QStringLiteral("not-found");
    }
    if (status_code == 429) {
        return QStringLiteral("rate-limited");
    }
    if (status_code >= 500) {
        return QStringLiteral("provider-unavailable");
    }
    return QStringLiteral("network-error");
}

[[nodiscard]] bool retryableStatus(const QString& status) {
    return status == QStringLiteral("rate-limited") || status == QStringLiteral("quota-exceeded")
           || status == QStringLiteral("provider-unavailable");
}

} // namespace

GoogleMapTilesService::GoogleMapTilesService(
    MapProviderPreferences* const preferences,
    QObject* const parent
) :
    GoogleMapTilesService(
        preferences,
        nullptr,
        QUrl(QStringLiteral("https://tile.googleapis.com")),
        parent
    ) {}

GoogleMapTilesService::GoogleMapTilesService(
    MapProviderPreferences* const preferences,
    QNetworkAccessManager* const network,
    const QUrl& base_url,
    QObject* const parent
) :
    QObject(parent), preferences_(preferences),
    network_(network != nullptr ? network : new QNetworkAccessManager(this)), base_url_(base_url) {
    Q_ASSERT(preferences_ != nullptr);
    const QLocale system_locale = QLocale::system();
    language_ = normalizedLanguage(system_locale.bcp47Name());
    region_ = normalizedRegion(QLocale::territoryToCode(system_locale.territory()));
    synchronizeMapType();
    retry_timer_.setSingleShot(true);
    connect(&retry_timer_, &QTimer::timeout, this, [this] {
        const QSet<GoogleMapTileId> retry_tiles = std::exchange(retry_tiles_, {});
        for (const GoogleMapTileId& tile : retry_tiles) {
            if (visible_tiles_.contains(tile)) {
                enqueueTile(tile);
            }
        }
        pumpTileQueue();
    });
    connect(
        preferences_,
        &MapProviderPreferences::googleMapTypeChanged,
        this,
        &GoogleMapTilesService::synchronizeMapType
    );
    connect(preferences_, &MapProviderPreferences::googleMapTilesAllowedChanged, this, [this] {
        if (!preferences_->googleMapTilesAllowed()) {
            setActive(false);
        }
    });
    connect(preferences_, &MapProviderPreferences::googleApiKeyStoredChanged, this, [this] {
        if (!preferences_->googleApiKeyStored()) {
            setActive(false);
        }
    });
}

GoogleMapTilesService::~GoogleMapTilesService() {
    ++generation_;
    abortRuntimeReplies();
}

bool GoogleMapTilesService::active() const noexcept {
    return active_;
}

bool GoogleMapTilesService::ready() const noexcept {
    return ready_;
}

bool GoogleMapTilesService::busy() const noexcept {
    return busy_;
}

QString GoogleMapTilesService::statusCode() const {
    return status_code_;
}

QString GoogleMapTilesService::copyrightText() const {
    return copyright_text_;
}

int GoogleMapTilesService::tileWidth() const noexcept {
    return session_.tile_width > 0 ? session_.tile_width : 256;
}

int GoogleMapTilesService::tileHeight() const noexcept {
    return session_.tile_height > 0 ? session_.tile_height : 256;
}

int GoogleMapTilesService::maximumZoom() const noexcept {
    return maximum_zoom_;
}

QString GoogleMapTilesService::language() const {
    return language_;
}

QString GoogleMapTilesService::region() const {
    return region_;
}

void GoogleMapTilesService::setActive(const bool active) {
    if (!active) {
        if (!active_ && api_key_.isEmpty()) {
            return;
        }
        active_ = false;
        ++generation_;
        clearRuntime();
        emit activeChanged();
        return;
    }
    if (active_) {
        return;
    }
    if (!preferences_->googleApiKeyStored() || !preferences_->googleMapTilesAllowed()) {
        setStatus(QStringLiteral("permission-required"));
        return;
    }
    const SecretStoreResult secret = preferences_->readGoogleApiKey();
    if (!secret.succeeded() || secret.value.isEmpty()) {
        setStatus(QStringLiteral("credential-unavailable"));
        return;
    }
    api_key_ = secret.value;
    active_ = true;
    ++generation_;
    setStatus({});
    emit activeChanged();
    beginSession();
}

void GoogleMapTilesService::setLanguage(const QString& language) {
    const QString normalized = normalizedLanguage(language);
    if (language_ == normalized) {
        return;
    }
    language_ = normalized;
    emit languageChanged();
    if (active_) {
        cache_.clear();
        visible_images_.clear();
        cached_bytes_ = 0;
        ++generation_;
        abortRuntimeReplies();
        session_ = {};
        setReady(false);
        beginSession();
    }
}

void GoogleMapTilesService::setRegion(const QString& region) {
    const QString normalized = normalizedRegion(region);
    if (region_ == normalized) {
        return;
    }
    region_ = normalized;
    emit regionChanged();
    if (active_) {
        cache_.clear();
        visible_images_.clear();
        cached_bytes_ = 0;
        ++generation_;
        abortRuntimeReplies();
        session_ = {};
        setReady(false);
        beginSession();
    }
}

void GoogleMapTilesService::updateViewport(
    const double north,
    const double south,
    const double east,
    const double west,
    const int zoom
) {
    viewport_ = {
        .north = std::clamp(north, -85.05112878, 85.05112878),
        .south = std::clamp(south, -85.05112878, 85.05112878),
        .east = std::clamp(east, -maximum_viewport_longitude, maximum_viewport_longitude),
        .west = std::clamp(west, -maximum_viewport_longitude, maximum_viewport_longitude),
        .zoom = std::clamp(zoom, 0, maximum_google_2d_zoom),
        .valid = std::isfinite(north) && std::isfinite(south) && std::isfinite(east)
                 && std::isfinite(west) && north >= south,
    };
    if (ready_ && viewport_.valid) {
        sendViewport();
    }
}

void GoogleMapTilesService::setVisibleTiles(const QSet<GoogleMapTileId>& tiles) {
    visible_tiles_ = tiles;
    for (auto it = visible_images_.begin(); it != visible_images_.end();) {
        if (!visible_tiles_.contains(it.key())) {
            it = visible_images_.erase(it);
        } else {
            ++it;
        }
    }
    QQueue<GoogleMapTileId> retained_queue;
    queued_tiles_.clear();
    while (!tile_queue_.isEmpty()) {
        const GoogleMapTileId tile = tile_queue_.dequeue();
        if (visible_tiles_.contains(tile)) {
            retained_queue.enqueue(tile);
            queued_tiles_.insert(tile);
        }
    }
    tile_queue_ = std::move(retained_queue);
    QList<QNetworkReply*> offscreen_replies;
    for (auto it = tile_replies_.cbegin(); it != tile_replies_.cend(); ++it) {
        if (!visible_tiles_.contains(it.value())) {
            offscreen_replies.push_back(it.key());
        }
    }
    for (QNetworkReply* const reply : offscreen_replies) {
        reply->abort();
    }
}

void GoogleMapTilesService::requestTile(const GoogleMapTileId& tile) {
    if (!active_ || !visible_tiles_.contains(tile)) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto cache = cache_.find(tile);
    if (cache != cache_.end()) {
        cache->last_use = ++use_counter_;
        if (cache->policy.freshAt(cache->stored_at_utc, now)) {
            visible_images_.insert(tile, cache->image);
            emit tileReady(tile.zoom, tile.x, tile.y);
            return;
        }
        if (cache->policy.staleUsableAt(cache->stored_at_utc, now)) {
            visible_images_.insert(tile, cache->image);
            emit tileReady(tile.zoom, tile.x, tile.y);
            enqueueTile(tile);
            pumpTileQueue();
            return;
        }
    }
    visible_images_.remove(tile);
    enqueueTile(tile);
    pumpTileQueue();
}

QImage GoogleMapTilesService::tileImage(const GoogleMapTileId& tile) const {
    const auto display = visible_images_.constFind(tile);
    if (display != visible_images_.constEnd()) {
        return *display;
    }
    const auto cache = cache_.constFind(tile);
    if (cache == cache_.constEnd()) {
        return {};
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (cache->policy.freshAt(cache->stored_at_utc, now)
        || cache->policy.staleUsableAt(cache->stored_at_utc, now)) {
        return cache->image;
    }
    return {};
}

void GoogleMapTilesService::beginSession() {
    if (!active_ || session_reply_ != nullptr) {
        return;
    }
    setReady(false);
    QUrl url = endpoint(QStringLiteral("/v1/createSession"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), api_key_);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(15000);
    const quint64 request_generation = generation_;
    session_reply_ = network_->post(request, makeSessionRequestBody(map_type_, language_, region_));
    connect(session_reply_, &QNetworkReply::finished, this, [this, request_generation] {
        QNetworkReply* const reply = session_reply_;
        session_reply_ = nullptr;
        finishSession(reply, request_generation);
    });
    refreshBusy();
}

void GoogleMapTilesService::finishSession(QNetworkReply* const reply, const quint64 generation) {
    if (reply == nullptr) {
        return;
    }
    const QByteArray payload = reply->readAll();
    const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool current = generation == generation_ && active_;
    reply->deleteLater();
    if (!current) {
        refreshBusy();
        return;
    }
    if (reply->error() != QNetworkReply::NoError || http_status != 200) {
        QString status = parseGoogleMapErrorReason(payload);
        if (status == QStringLiteral("provider-error")) {
            status = statusForHttpCode(http_status);
        }
        setStatus(status);
        refreshBusy();
        return;
    }
    QString parse_error;
    const auto session = parseSessionResponse(payload, &parse_error);
    if (!session.has_value()) {
        setStatus(parse_error);
        refreshBusy();
        return;
    }
    session_ = *session;
    retry_delay_ms_ = 1000;
    setStatus({});
    setReady(true);
    emit sessionChanged();
    if (viewport_.valid) {
        sendViewport();
    }
    pumpTileQueue();
    refreshBusy();
}

void GoogleMapTilesService::sendViewport() {
    if (!active_ || !ready_ || !viewport_.valid) {
        return;
    }
    if (viewport_reply_ != nullptr) {
        QNetworkReply* const superseded_reply = viewport_reply_;
        viewport_reply_ = nullptr;
        disconnect(superseded_reply, nullptr, this, nullptr);
        superseded_reply->abort();
        superseded_reply->deleteLater();
    }
    QUrl url = endpoint(QStringLiteral("/tile/v1/viewport"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("session"), session_.token);
    query.addQueryItem(QStringLiteral("key"), api_key_);
    query.addQueryItem(QStringLiteral("zoom"), QString::number(viewport_.zoom));
    query.addQueryItem(QStringLiteral("north"), QString::number(viewport_.north, 'f', 7));
    query.addQueryItem(QStringLiteral("south"), QString::number(viewport_.south, 'f', 7));
    query.addQueryItem(QStringLiteral("east"), QString::number(viewport_.east, 'f', 7));
    query.addQueryItem(QStringLiteral("west"), QString::number(viewport_.west, 'f', 7));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    const quint64 request_generation = generation_;
    viewport_reply_ = network_->get(request);
    connect(viewport_reply_, &QNetworkReply::finished, this, [this, request_generation] {
        QNetworkReply* const reply = viewport_reply_;
        viewport_reply_ = nullptr;
        finishViewport(reply, request_generation);
    });
    refreshBusy();
}

void GoogleMapTilesService::finishViewport(QNetworkReply* const reply, const quint64 generation) {
    if (reply == nullptr) {
        return;
    }
    const QByteArray payload = reply->readAll();
    const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    const bool current = generation == generation_ && active_;
    reply->deleteLater();
    if (!current) {
        refreshBusy();
        return;
    }
    if (network_error != QNetworkReply::NoError || http_status != 200) {
        QString status = parseGoogleMapErrorReason(payload);
        if (status == QStringLiteral("provider-error")) {
            status = statusForHttpCode(http_status);
        }
        setStatus(status);
        if (status == QStringLiteral("session-expired")) {
            beginSession();
        }
        refreshBusy();
        return;
    }
    QString parse_error;
    const auto viewport = parseViewportResponse(payload, &parse_error);
    if (!viewport.has_value()) {
        setStatus(parse_error);
        refreshBusy();
        return;
    }
    setStatus({});
    bool changed = copyright_text_ != viewport->copyright_text;
    copyright_text_ = viewport->copyright_text;
    const int maximum_zoom = viewport->maximum_zoom.value_or(22);
    changed |= maximum_zoom_ != maximum_zoom;
    maximum_zoom_ = maximum_zoom;
    if (changed) {
        emit viewportInfoChanged();
    }
    pumpTileQueue();
    refreshBusy();
}

void GoogleMapTilesService::enqueueTile(const GoogleMapTileId& tile) {
    if (!visible_tiles_.contains(tile) || queued_tiles_.contains(tile)
        || tile_replies_.values().contains(tile)) {
        return;
    }
    tile_queue_.enqueue(tile);
    queued_tiles_.insert(tile);
}

void GoogleMapTilesService::pumpTileQueue() {
    if (!active_ || !ready_ || copyright_text_.isEmpty() || sessionExpired()) {
        if (active_ && sessionExpired()) {
            beginSession();
        }
        return;
    }
    while (tile_replies_.size() < maximum_parallel_tile_requests && !tile_queue_.isEmpty()) {
        const GoogleMapTileId tile = tile_queue_.dequeue();
        queued_tiles_.remove(tile);
        if (!visible_tiles_.contains(tile)) {
            continue;
        }
        QUrl url =
            endpoint(QStringLiteral("/v1/2dtiles/%1/%2/%3").arg(tile.zoom).arg(tile.x).arg(tile.y));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("session"), session_.token);
        query.addQueryItem(QStringLiteral("key"), api_key_);
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setTransferTimeout(15000);
        const auto cache = cache_.constFind(tile);
        if (cache != cache_.constEnd() && !cache->policy.etag.isEmpty()) {
            request.setRawHeader("If-None-Match", cache->policy.etag);
        }
        QNetworkReply* const reply = network_->get(request);
        tile_replies_.insert(reply, tile);
        const quint64 request_generation = generation_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, request_generation] {
            finishTile(reply, request_generation);
        });
    }
    refreshBusy();
}

void GoogleMapTilesService::finishTile(QNetworkReply* const reply, const quint64 generation) {
    const auto reply_entry = tile_replies_.find(reply);
    if (reply_entry == tile_replies_.end()) {
        reply->deleteLater();
        return;
    }
    const GoogleMapTileId tile = *reply_entry;
    tile_replies_.erase(reply_entry);
    const QByteArray payload = reply->readAll();
    const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    const QByteArray cache_control = reply->rawHeader("Cache-Control");
    const QByteArray etag = reply->rawHeader("ETag");
    const bool current = generation == generation_ && active_;
    reply->deleteLater();
    if (!current) {
        pumpTileQueue();
        return;
    }

    if (network_error == QNetworkReply::OperationCanceledError) {
        pumpTileQueue();
        return;
    }

    if (http_status == 304) {
        auto cache = cache_.find(tile);
        if (cache != cache_.end()) {
            HttpCachePolicy policy =
                cache_control.isEmpty() ? cache->policy : parseHttpCachePolicy(cache_control, etag);
            if (!policy.no_store && !etag.trimmed().isEmpty()) {
                policy.etag = etag.trimmed();
            } else if (!policy.no_store && policy.etag.isEmpty()) {
                policy.etag = cache->policy.etag;
            }
            const QImage revalidated_image = cache->image;
            if (policy.no_store) {
                cached_bytes_ -= revalidated_image.sizeInBytes();
                cache_.erase(cache);
                if (visible_tiles_.contains(tile)) {
                    visible_images_.insert(tile, revalidated_image);
                }
            } else {
                cache->policy = policy;
                cache->stored_at_utc = QDateTime::currentDateTimeUtc();
                cache->last_use = ++use_counter_;
                if (visible_tiles_.contains(tile)) {
                    visible_images_.insert(tile, revalidated_image);
                } else {
                    visible_images_.remove(tile);
                }
            }
            retry_attempts_.remove(tile);
            retry_delay_ms_ = 1000;
            emit tileReady(tile.zoom, tile.x, tile.y);
        }
        pumpTileQueue();
        return;
    }

    if (network_error != QNetworkReply::NoError || http_status != 200) {
        QString status = parseGoogleMapErrorReason(payload);
        if (status == QStringLiteral("provider-error")) {
            status = statusForHttpCode(http_status);
        }
        setStatus(status);
        if (status == QStringLiteral("session-expired")) {
            enqueueTile(tile);
            session_ = {};
            setReady(false);
            beginSession();
        } else if (retryableStatus(status)) {
            scheduleRetry(tile);
        } else {
            emit tileUnavailable(tile.zoom, tile.x, tile.y);
        }
        pumpTileQueue();
        return;
    }

    QImage image;
    if (!image.loadFromData(payload) || image.isNull()) {
        setStatus(QStringLiteral("invalid-tile-image"));
        emit tileUnavailable(tile.zoom, tile.x, tile.y);
        pumpTileQueue();
        return;
    }
    if (image.width() != tileWidth() || image.height() != tileHeight()
        || image.sizeInBytes() > 16 * 1024 * 1024) {
        setStatus(QStringLiteral("invalid-tile-image"));
        emit tileUnavailable(tile.zoom, tile.x, tile.y);
        pumpTileQueue();
        return;
    }
    const HttpCachePolicy policy = parseHttpCachePolicy(cache_control, etag);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (policy.no_store) {
        const auto existing = cache_.find(tile);
        if (existing != cache_.end()) {
            cached_bytes_ -= existing->image.sizeInBytes();
            cache_.erase(existing);
        }
        if (visible_tiles_.contains(tile)) {
            visible_images_.insert(tile, image);
        }
    } else {
        auto existing = cache_.find(tile);
        if (existing != cache_.end()) {
            cached_bytes_ -= existing->image.sizeInBytes();
        }
        cache_.insert(
            tile,
            CachedTile{
                .image = image,
                .policy = policy,
                .stored_at_utc = now,
                .last_use = ++use_counter_,
            }
        );
        cached_bytes_ += image.sizeInBytes();
        if (visible_tiles_.contains(tile)) {
            visible_images_.insert(tile, image);
        } else {
            visible_images_.remove(tile);
        }
        evictCache();
    }
    retry_attempts_.remove(tile);
    retry_delay_ms_ = 1000;
    setStatus({});
    emit tileReady(tile.zoom, tile.x, tile.y);
    pumpTileQueue();
}

void GoogleMapTilesService::scheduleRetry(const GoogleMapTileId& tile) {
    const int attempts = retry_attempts_.value(tile, 0) + 1;
    retry_attempts_.insert(tile, attempts);
    if (attempts > maximum_retry_attempts) {
        emit tileUnavailable(tile.zoom, tile.x, tile.y);
        return;
    }
    retry_tiles_.insert(tile);
    if (!retry_timer_.isActive()) {
        retry_timer_.start(retry_delay_ms_);
        retry_delay_ms_ = std::min(retry_delay_ms_ * 2, 32000);
    }
}

void GoogleMapTilesService::clearRuntime() {
    retry_timer_.stop();
    abortRuntimeReplies();
    api_key_.clear();
    session_ = {};
    viewport_ = {};
    copyright_text_.clear();
    maximum_zoom_ = 22;
    tile_queue_.clear();
    queued_tiles_.clear();
    visible_tiles_.clear();
    retry_tiles_.clear();
    retry_attempts_.clear();
    cache_.clear();
    visible_images_.clear();
    cached_bytes_ = 0;
    retry_delay_ms_ = 1000;
    setReady(false);
    setStatus({});
    emit sessionChanged();
    emit viewportInfoChanged();
    refreshBusy();
}

void GoogleMapTilesService::abortRuntimeReplies() {
    if (session_reply_ != nullptr) {
        disconnect(session_reply_, nullptr, this, nullptr);
        session_reply_->abort();
        session_reply_->deleteLater();
        session_reply_ = nullptr;
    }
    if (viewport_reply_ != nullptr) {
        disconnect(viewport_reply_, nullptr, this, nullptr);
        viewport_reply_->abort();
        viewport_reply_->deleteLater();
        viewport_reply_ = nullptr;
    }
    const QList<QNetworkReply*> replies = tile_replies_.keys();
    tile_replies_.clear();
    for (QNetworkReply* const reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    refreshBusy();
}

void GoogleMapTilesService::evictCache() {
    while (cached_bytes_ > maximum_cached_bytes && !cache_.isEmpty()) {
        auto oldest = cache_.begin();
        for (auto it = std::next(cache_.begin()); it != cache_.end(); ++it) {
            if (it->last_use < oldest->last_use) {
                oldest = it;
            }
        }
        cached_bytes_ -= oldest->image.sizeInBytes();
        cache_.erase(oldest);
    }
}

void GoogleMapTilesService::refreshBusy() {
    const bool busy =
        session_reply_ != nullptr || viewport_reply_ != nullptr || !tile_replies_.isEmpty();
    if (busy_ == busy) {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void GoogleMapTilesService::setReady(const bool ready) {
    if (ready_ == ready) {
        return;
    }
    ready_ = ready;
    emit readyChanged();
}

void GoogleMapTilesService::setStatus(const QString& status) {
    if (status_code_ == status) {
        return;
    }
    status_code_ = status;
    emit statusChanged();
}

void GoogleMapTilesService::synchronizeMapType() {
    const QString map_type = preferences_->googleMapType();
    if (map_type_ == map_type) {
        return;
    }
    map_type_ = map_type;
    cache_.clear();
    visible_images_.clear();
    cached_bytes_ = 0;
    if (active_) {
        ++generation_;
        abortRuntimeReplies();
        session_ = {};
        setReady(false);
        beginSession();
    }
}

QUrl GoogleMapTilesService::endpoint(const QString& path) const {
    QUrl url = base_url_;
    url.setPath(path);
    url.setQuery(QString{});
    url.setFragment(QString{});
    return url;
}

bool GoogleMapTilesService::sessionExpired() const {
    return session_.token.isEmpty() || !session_.expires_at_utc.isValid()
           || session_.expires_at_utc <= QDateTime::currentDateTimeUtc().addSecs(5);
}

} // namespace shadow::desktop::maps
