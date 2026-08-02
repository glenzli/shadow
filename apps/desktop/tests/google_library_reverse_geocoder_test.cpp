#include "google_library_reverse_geocoder.hpp"

#include "map_provider_preferences.hpp"
#include "secure_secret_store.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
#include <optional>
#include <utility>

namespace {

struct FakeSecretState final {
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
        const int status,
        const int delay_ms,
        QObject* const parent
    ) : QNetworkReply(parent), payload_(std::move(payload)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(delay_ms, this, [this]() {
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
        std::memcpy(data, payload_.constData() + offset_, static_cast<std::size_t>(count));
        offset_ += count;
        return count;
    }

  private:
    QByteArray payload_;
    qint64 offset_ = 0;
    bool finished_ = false;
};

class FakeGeocodingNetwork final : public QNetworkAccessManager {
  public:
    QUrl last_url;
    int request_count = 0;
    int next_status = 200;
    int next_delay_ms = 0;
    QByteArray next_payload = QByteArrayLiteral(R"json(
        {
          "status": "OK",
          "results": [{
            "formatted_address": "黄石国家公园, WY 82190, 美国",
            "address_components": [
              {"long_name":"美国","short_name":"US","types":["country"]},
              {"long_name":"怀俄明州","short_name":"WY","types":["administrative_area_level_1"]},
              {"long_name":"黄石国家公园","short_name":"黄石国家公园","types":["locality","political"]}
            ]
          }]
        }
    )json");

  protected:
    QNetworkReply*
    createRequest(const Operation operation, const QNetworkRequest& request, QIODevice*) override {
        ++request_count;
        last_url = request.url();
        return new FakeNetworkReply(
            request,
            operation,
            next_payload,
            next_status,
            next_delay_ms,
            this
        );
    }
};

[[nodiscard]] bool waitUntil(const std::function<bool()>& condition, const int timeout_ms = 2'000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}

[[nodiscard]] bool expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    bool passed = expect(root.isValid(), "temporary settings root is unavailable");
    auto secret_state = std::make_shared<FakeSecretState>();
    MapProviderPreferences preferences(
        root.filePath(QStringLiteral("preferences.ini")),
        std::make_unique<FakeSecretStore>(secret_state)
    );
    FakeGeocodingNetwork network;
    auto provider = makeGoogleLibraryReverseGeocoder(
        &preferences,
        &network,
        QUrl(QStringLiteral("https://geocode.test.invalid/maps/api/geocode/json"))
    );

    passed &= expect(!provider->available(), "provider started without explicit permission");
    passed &= expect(network.request_count == 0, "provider contacted Google before authorization");
    passed &= expect(
        preferences.storeGoogleApiKey(QStringLiteral("AIzaShadowRuntimeContract_123456789")),
        "test credential could not be stored"
    );
    passed &= expect(!provider->available(), "storing a key implicitly enabled geocoding");
    preferences.setGoogleReverseGeocodingAllowed(true);
    passed &= expect(provider->available(), "explicit geocoding permission was not observed");

    bool completed = false;
    std::optional<BackendLibraryPlaceResolutionResult> result;
    QString error;
    provider->reverseGeocode(
        BackendLibraryPlaceResolutionCandidate{
            .latitude_e7 = 446'198'000,
            .longitude_e7 = -1'104'255'267,
            .photo_count = 1,
        },
        [&completed,
         &result,
         &error](std::optional<BackendLibraryPlaceResolutionResult> value, QString diagnostic) {
            completed = true;
            result = std::move(value);
            error = std::move(diagnostic);
        }
    );
    passed &= expect(waitUntil([&completed]() { return completed; }), "request did not complete");
    const QUrlQuery query(network.last_url);
    passed &= expect(network.request_count == 1, "request count changed");
    passed &= expect(
        query.queryItemValue(QStringLiteral("latlng")) == QStringLiteral("44.6198000,-110.4255267"),
        "coordinate query changed"
    );
    passed &= expect(
        query.queryItemValue(QStringLiteral("key"))
            == QStringLiteral("AIzaShadowRuntimeContract_123456789"),
        "stored key did not authorize the provider request"
    );
    passed &= expect(error.isEmpty() && result.has_value(), "valid response did not resolve");
    if (result) {
        passed &= expect(
            result->country_code == QStringLiteral("US")
                && result->country_name == QStringLiteral("美国")
                && result->administrative_area == QStringLiteral("怀俄明州")
                && result->locality == QStringLiteral("黄石国家公园")
                && result->provider_id == QStringLiteral("google-geocoding")
                && result->provider_version == QStringLiteral("v1"),
            "structured address projection changed"
        );
    }

    network.next_payload =
        QByteArrayLiteral(R"({"status":"REQUEST_DENIED","error_message":"do not expose secrets"})");
    completed = false;
    result.reset();
    error.clear();
    provider->reverseGeocode(
        BackendLibraryPlaceResolutionCandidate{
            .latitude_e7 = 434'780'983,
            .longitude_e7 = -1'108'047'084,
            .photo_count = 1,
        },
        [&completed,
         &result,
         &error](std::optional<BackendLibraryPlaceResolutionResult> value, QString diagnostic) {
            completed = true;
            result = std::move(value);
            error = std::move(diagnostic);
        }
    );
    passed &= expect(
        waitUntil([&completed]() { return completed; }) && !result && error.contains("billing"),
        "provider rejection did not produce an actionable safe diagnostic"
    );

    network.next_payload = QByteArrayLiteral(R"({"status":"OK","results":[]})");
    network.next_delay_ms = 200;
    completed = false;
    result.reset();
    error.clear();
    provider->reverseGeocode(
        BackendLibraryPlaceResolutionCandidate{
            .latitude_e7 = 312'304'000,
            .longitude_e7 = 1'214'737'000,
            .photo_count = 1,
        },
        [&completed,
         &result,
         &error](std::optional<BackendLibraryPlaceResolutionResult> value, QString diagnostic) {
            completed = true;
            result = std::move(value);
            error = std::move(diagnostic);
        }
    );
    preferences.setGoogleReverseGeocodingAllowed(false);
    passed &= expect(
        completed && !result && error.contains(QStringLiteral("authorization")),
        "revoking permission left an in-flight request without completion"
    );
    passed &= expect(!provider->available(), "revoked permission left the provider available");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
