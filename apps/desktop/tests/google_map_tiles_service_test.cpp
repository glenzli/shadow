#include "map/google_map_tile_layer.hpp"
#include "map/google_map_tiles_service.hpp"
#include "map_provider_preferences.hpp"

#include <QBuffer>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using shadow::desktop::maps::GoogleMapTileId;
using shadow::desktop::maps::GoogleMapTileLayer;
using shadow::desktop::maps::GoogleMapTilesService;

struct FakeSecretState {
    QString value;
    SecretStoreStatus status = SecretStoreStatus::NotFound;
};

class FakeSecretStore final : public SecretStore {
  public:
    explicit FakeSecretStore(std::shared_ptr<FakeSecretState> state) : state_(std::move(state)) {}

    [[nodiscard]] bool available() const noexcept override {
        return true;
    }

    [[nodiscard]] SecretStoreResult read(const QString&, const QString&) const override {
        return {.status = state_->status, .value = state_->value};
    }

    [[nodiscard]] SecretStoreResult
    write(const QString&, const QString&, const QString& value) override {
        state_->value = value;
        state_->status = SecretStoreStatus::Success;
        return {.status = SecretStoreStatus::Success};
    }

    [[nodiscard]] SecretStoreResult remove(const QString&, const QString&) override {
        state_->value.clear();
        state_->status = SecretStoreStatus::NotFound;
        return {.status = SecretStoreStatus::Success};
    }

  private:
    std::shared_ptr<FakeSecretState> state_;
};

class FakeNetworkReply final : public QNetworkReply {
  public:
    FakeNetworkReply(
        const QNetworkRequest& request,
        const QNetworkAccessManager::Operation operation,
        QByteArray payload,
        const QByteArray& content_type,
        const QByteArray& cache_control,
        const QByteArray& etag,
        const int status,
        QObject* const parent
    ) : QNetworkReply(parent), payload_(std::move(payload)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, content_type);
        setRawHeader("Cache-Control", cache_control);
        if (!etag.isEmpty()) {
            setRawHeader("ETag", etag);
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (finished_) {
                return;
            }
            finished_ = true;
            emit readyRead();
            emit finished();
        });
    }

    void abort() override {
        if (finished_) {
            return;
        }
        finished_ = true;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("cancelled"));
        emit finished();
    }

    [[nodiscard]] qint64 bytesAvailable() const override {
        return static_cast<qint64>(payload_.size()) - offset_ + QNetworkReply::bytesAvailable();
    }

  protected:
    qint64 readData(char* const data, const qint64 maximum_size) override {
        if (offset_ >= payload_.size()) {
            return -1;
        }
        const qint64 available = payload_.size() - offset_;
        const qint64 count = std::min(maximum_size, available);
        std::memcpy(data, payload_.constData() + offset_, static_cast<size_t>(count));
        offset_ += count;
        return count;
    }

  private:
    QByteArray payload_;
    qint64 offset_ = 0;
    bool finished_ = false;
};

class FakeTileNetwork final : public QNetworkAccessManager {
  public:
    FakeTileNetwork() {
        QImage tile(256, 256, QImage::Format_RGB32);
        tile.fill(QColor(34, 139, 94));
        QBuffer buffer(&tile_payload_);
        buffer.open(QIODevice::WriteOnly);
        tile.save(&buffer, "PNG");
    }

    [[nodiscard]] int sessionRequests() const noexcept {
        return session_requests_;
    }

    [[nodiscard]] int viewportRequests() const noexcept {
        return viewport_requests_;
    }

    [[nodiscard]] int tileRequests() const noexcept {
        return tile_requests_;
    }

    [[nodiscard]] QByteArray sessionBody() const {
        return session_body_;
    }
    [[nodiscard]] int conditionalTileRequests() const noexcept {
        return conditional_tile_requests_;
    }
    [[nodiscard]] int expiredTileResponses() const noexcept {
        return expired_tile_responses_;
    }
    [[nodiscard]] int maximumRequestedTileZoom() const noexcept {
        return maximum_requested_tile_zoom_;
    }
    [[nodiscard]] QUrl viewportUrl() const {
        return viewport_url_;
    }
    void failNextViewport() {
        ++viewport_failures_remaining_;
    }

  protected:
    QNetworkReply* createRequest(
        const Operation operation,
        const QNetworkRequest& request,
        QIODevice* const outgoing_data
    ) override {
        const QString path = request.url().path();
        QByteArray payload;
        QByteArray content_type = "application/json";
        QByteArray cache_control = "no-store";
        QByteArray etag;
        int status = 200;
        if (path == QStringLiteral("/v1/createSession")) {
            ++session_requests_;
            if (outgoing_data != nullptr) {
                session_body_ = outgoing_data->readAll();
            }
            payload = QByteArrayLiteral("{\"session\":\"local-session\",\"expiry\":")
                      + QByteArray::number(QDateTime::currentSecsSinceEpoch() + 3600)
                      + QByteArrayLiteral(
                          ",\"tileWidth\":256,\"tileHeight\":256,"
                          "\"imageFormat\":\"png\"}"
                      );
        } else if (path == QStringLiteral("/tile/v1/viewport")) {
            ++viewport_requests_;
            viewport_url_ = request.url();
            if (viewport_failures_remaining_ > 0) {
                --viewport_failures_remaining_;
                payload = QByteArrayLiteral(R"({"error":{}})");
                status = 503;
            } else {
                payload = QByteArrayLiteral(
                    "{\"copyright\":\"Map data local contract\","
                    "\"maxZoomRects\":[{\"maxZoom\":19}]}"
                );
            }
        } else if (path.startsWith(QStringLiteral("/v1/2dtiles/"))) {
            ++tile_requests_;
            bool valid_zoom = false;
            const int requested_zoom = path.split('/').value(3).toInt(&valid_zoom);
            if (valid_zoom) {
                maximum_requested_tile_zoom_ =
                    std::max(maximum_requested_tile_zoom_, requested_zoom);
            }
            if (path.endsWith(QStringLiteral("/2/0/1")) && expired_tile_responses_ == 0) {
                ++expired_tile_responses_;
                payload = QByteArrayLiteral("{\"error\":{\"details\":[{\"reason\":\"expired\"}]}}");
                status = 400;
            } else if (path.endsWith(QStringLiteral("/2/3/1"))
                       && !request.rawHeader("If-None-Match").isEmpty()) {
                ++conditional_tile_requests_;
                cache_control = "no-store";
                status = 304;
            } else {
                payload = tile_payload_;
                content_type = "image/png";
                cache_control = path.endsWith(QStringLiteral("/2/1/1"))
                                    ? QByteArrayLiteral("no-store")
                                : path.endsWith(QStringLiteral("/2/3/1"))
                                    ? QByteArrayLiteral("private, max-age=0")
                                    : QByteArrayLiteral("private, max-age=120");
                etag = "\"local-tile-v1\"";
            }
        } else {
            payload = QByteArrayLiteral("{\"error\":{\"details\":[{\"reason\":\"notFound\"}]}}");
            status = 404;
        }
        return new FakeNetworkReply(
            request,
            operation,
            std::move(payload),
            content_type,
            cache_control,
            etag,
            status,
            this
        );
    }

    QByteArray tile_payload_;
    QByteArray session_body_;
    QUrl viewport_url_;
    int session_requests_ = 0;
    int viewport_requests_ = 0;
    int tile_requests_ = 0;
    int conditional_tile_requests_ = 0;
    int expired_tile_responses_ = 0;
    int maximum_requested_tile_zoom_ = -1;
    int viewport_failures_remaining_ = 0;
};

[[nodiscard]] bool waitUntil(const std::function<bool()>& predicate, const int timeout_ms = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return predicate();
}

[[nodiscard]] bool expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory is unavailable")) {
        return EXIT_FAILURE;
    }

    auto secret_state = std::make_shared<FakeSecretState>();
    MapProviderPreferences preferences(
        root.filePath(QStringLiteral("preferences.ini")),
        std::make_unique<FakeSecretStore>(secret_state)
    );
    const QString api_key = QStringLiteral("AIzaShadowRuntimeContract_123456789");
    if (!expect(
            preferences.storeGoogleApiKey(api_key),
            "fake credential store rejected the test key"
        )) {
        return EXIT_FAILURE;
    }
    preferences.setGoogleMapTilesAllowed(true);
    preferences.setGoogleMapType(QStringLiteral("terrain"));

    FakeTileNetwork network;
    GoogleMapTilesService service(
        &preferences,
        &network,
        QUrl(QStringLiteral("https://tile.test.invalid"))
    );
    service.setLanguage(QStringLiteral("zh-CN"));
    service.setRegion(QStringLiteral("CN"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    bool passed = expect(
        network.sessionRequests() == 0 && network.tileRequests() == 0
            && network.viewportRequests() == 0,
        "storing permissions or selecting Google contacted the provider"
    );
    service.setActive(true);

    passed &=
        expect(waitUntil([&service] { return service.ready(); }), "session did not become ready");
    passed &= expect(network.sessionRequests() == 1, "session request count changed");
    passed &= expect(
        network.sessionBody().contains("\"mapType\":\"terrain\"")
            && network.sessionBody().contains("\"layerRoadmap\":true")
            && !network.sessionBody().contains(api_key.toUtf8()),
        "session request body changed or exposed the key"
    );

    const GoogleMapTileId direct_tile{.zoom = 2, .x = 2, .y = 1};
    service.setVisibleTiles(QSet<GoogleMapTileId>{direct_tile});
    service.requestTile(direct_tile);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    passed &= expect(
        network.tileRequests() == 0,
        "tiles were requested before viewport attribution was available"
    );

    service.updateViewport(90.0, -90.0, 180.0, -180.0, 99);
    passed &= expect(
        waitUntil([&service] { return !service.copyrightText().isEmpty(); }),
        "viewport attribution did not arrive"
    );
    const QUrlQuery viewport_query(network.viewportUrl());
    passed &= expect(
        service.copyrightText() == QStringLiteral("Map data local contract")
            && service.maximumZoom() == 19 && network.viewportRequests() == 1
            && viewport_query.queryItemValue(QStringLiteral("zoom")) == QStringLiteral("22")
            && viewport_query.queryItemValue(QStringLiteral("east")).toDouble() < 180.0
            && viewport_query.queryItemValue(QStringLiteral("west")).toDouble() > -180.0,
        "viewport contract changed"
    );
    network.failNextViewport();
    service.updateViewport(40.0, 20.0, 130.0, 110.0, 7);
    passed &= expect(
        waitUntil([&service] {
            return service.statusCode() == QStringLiteral("provider-unavailable");
        }),
        "viewport HTTP failure did not reach the stable provider status"
    );
    service.updateViewport(40.0, 20.0, 130.0, 110.0, 7);
    passed &= expect(
        waitUntil([&service, &network] {
            return network.viewportRequests() == 3 && service.statusCode().isEmpty();
        }),
        "a successful viewport refresh did not clear the provider status"
    );

    passed &= expect(
        waitUntil([&service, direct_tile] { return !service.tileImage(direct_tile).isNull(); }),
        "visible tile did not arrive"
    );
    const int tile_requests_after_first = network.tileRequests();
    service.requestTile(direct_tile);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    passed &= expect(
        network.tileRequests() == tile_requests_after_first,
        "fresh memory cache triggered another request"
    );
    passed &= expect(network.cache() == nullptr, "the service unexpectedly installed a disk cache");

    const GoogleMapTileId no_store_tile{.zoom = 2, .x = 1, .y = 1};
    service.setVisibleTiles(QSet<GoogleMapTileId>{no_store_tile});
    service.requestTile(no_store_tile);
    passed &= expect(
        waitUntil([&service, no_store_tile] { return !service.tileImage(no_store_tile).isNull(); }),
        "no-store tile was not available for its active display"
    );
    service.setVisibleTiles({});
    passed &= expect(
        service.tileImage(no_store_tile).isNull(),
        "no-store tile survived after leaving the visible set"
    );
    const int no_store_request_count = network.tileRequests();
    service.setVisibleTiles(QSet<GoogleMapTileId>{no_store_tile});
    service.requestTile(no_store_tile);
    passed &= expect(
        waitUntil([&network, no_store_request_count] {
            return network.tileRequests() > no_store_request_count;
        }),
        "no-store tile was reused as an offline cache entry"
    );

    const GoogleMapTileId revalidated_tile{.zoom = 2, .x = 3, .y = 1};
    service.setVisibleTiles(QSet<GoogleMapTileId>{revalidated_tile});
    service.requestTile(revalidated_tile);
    passed &= expect(
        waitUntil([&service, revalidated_tile] {
            return !service.tileImage(revalidated_tile).isNull();
        }),
        "immediately stale tile was not displayed after retrieval"
    );
    service.requestTile(revalidated_tile);
    passed &= expect(
        waitUntil([&network, &service, revalidated_tile] {
            return network.conditionalTileRequests() == 1
                   && !service.tileImage(revalidated_tile).isNull();
        }),
        "stale ETag tile was not conditionally revalidated"
    );
    service.setVisibleTiles({});
    passed &= expect(
        service.tileImage(revalidated_tile).isNull(),
        "a no-store revalidation survived after leaving the visible set"
    );
    const int revalidated_request_count = network.tileRequests();
    service.setVisibleTiles(QSet<GoogleMapTileId>{revalidated_tile});
    service.requestTile(revalidated_tile);
    passed &= expect(
        waitUntil([&network, revalidated_request_count] {
            return network.tileRequests() > revalidated_request_count;
        }),
        "a no-store revalidation was reused as a cache entry"
    );

    const int sessions_before_expiry = network.sessionRequests();
    const GoogleMapTileId expired_session_tile{.zoom = 2, .x = 0, .y = 1};
    service.setVisibleTiles(QSet<GoogleMapTileId>{expired_session_tile});
    service.requestTile(expired_session_tile);
    passed &= expect(
        waitUntil([&service, expired_session_tile, &network, sessions_before_expiry] {
            return network.sessionRequests() > sessions_before_expiry
                   && !service.tileImage(expired_session_tile).isNull();
        }) && network.expiredTileResponses() == 1,
        "expired tile session was not renewed exactly once"
    );

    QSet<GoogleMapTileId> large_visible_set;
    for (int x = 100; x < 360; ++x) {
        large_visible_set.insert(GoogleMapTileId{.zoom = 10, .x = x, .y = 1});
    }
    const GoogleMapTileId first_large_visible_tile{.zoom = 10, .x = 100, .y = 1};
    const int requests_before_large_set = network.tileRequests();
    service.setVisibleTiles(large_visible_set);
    for (const GoogleMapTileId& tile : large_visible_set) {
        service.requestTile(tile);
    }
    passed &= expect(
        waitUntil(
            [&service, &network, &large_visible_set, requests_before_large_set] {
                return !service.busy()
                       && network.tileRequests()
                              >= requests_before_large_set + large_visible_set.size();
            },
            10000
        ) && !service.tileImage(first_large_visible_tile).isNull(),
        "the bounded cache evicted pixels that were still in the visible working set"
    );
    service.setVisibleTiles({});

    GoogleMapTileLayer layer;
    layer.setService(&service);
    layer.setWidth(256);
    layer.setHeight(256);
    layer.setCenterLatitude(0.0);
    layer.setCenterLongitude(0.0);
    layer.setZoomLevel(22.0);
    layer.setActive(true);
    passed &= expect(
        waitUntil([&network, tile_requests_after_first] {
            return network.tileRequests() > tile_requests_after_first;
        }),
        "tile layer did not request its visible set"
    );
    passed &= expect(
        waitUntil([&service] { return !service.busy(); }),
        "tile layer requests did not settle"
    );
    passed &= expect(
        network.maximumRequestedTileZoom() == 19,
        "tile layer ignored the viewport maximum zoom"
    );
    QImage rendered(256, 256, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    layer.paint(&painter);
    painter.end();
    passed &= expect(
        qAlpha(rendered.pixel(128, 128)) > 0,
        "tile layer did not paint a decoded visible tile"
    );

    layer.setActive(false);
    service.setActive(false);
    passed &= expect(
        !service.active() && !service.ready() && service.tileImage(direct_tile).isNull()
            && service.copyrightText().isEmpty(),
        "deactivation retained network or display state"
    );

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
