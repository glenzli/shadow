#include "amap_library_reverse_geocoder.hpp"

#include "map_provider_preferences.hpp"
#include "secure_secret_store.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
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

class FakeSecretStore final : public SecretStore {
  public:
    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] SecretStoreResult read(const QString&, const QString& account) const override {
        const auto found = values_.constFind(account);
        return found == values_.cend()
                   ? SecretStoreResult{.status = SecretStoreStatus::NotFound}
                   : SecretStoreResult{
                         .status = SecretStoreStatus::Success,
                         .value = found.value(),
                     };
    }
    [[nodiscard]] SecretStoreResult
    write(const QString&, const QString& account, const QString& value) override {
        values_.insert(account, value);
        return {.status = SecretStoreStatus::Success};
    }
    [[nodiscard]] SecretStoreResult remove(const QString&, const QString& account) override {
        values_.remove(account);
        return {.status = SecretStoreStatus::Success};
    }

  private:
    QHash<QString, QString> values_;
};

class FakeReply final : public QNetworkReply {
  public:
    FakeReply(const QNetworkRequest& request, QByteArray payload, QObject* parent) :
        QNetworkReply(parent), payload_(std::move(payload)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() { emit finished(); });
    }
    void abort() override { emit finished(); }
    [[nodiscard]] qint64 bytesAvailable() const override {
        return static_cast<qint64>(payload_.size()) - offset_ + QNetworkReply::bytesAvailable();
    }

  protected:
    qint64 readData(char* data, qint64 maximum_size) override {
        if (offset_ >= payload_.size()) {
            return -1;
        }
        const qint64 count = std::min(maximum_size, payload_.size() - offset_);
        std::memcpy(data, payload_.constData() + offset_, static_cast<std::size_t>(count));
        offset_ += count;
        return count;
    }

  private:
    QByteArray payload_;
    qint64 offset_ = 0;
};

class FakeNetwork final : public QNetworkAccessManager {
  public:
    QUrl last_url;
    int request_count = 0;
    QByteArray payload = QByteArrayLiteral(R"json({
      "status":"1","info":"OK","infocode":"10000",
      "regeocode":{
        "formatted_address":"北京市东城区东华门街道天安门",
        "addressComponent":{
          "country":"中国","province":"北京市","city":[],
          "district":"东城区","adcode":"110101"
        }
      }
    })json");

  protected:
    QNetworkReply*
    createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        ++request_count;
        last_url = request.url();
        return new FakeReply(request, payload, this);
    }
};

[[nodiscard]] bool waitUntil(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 2'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}

[[nodiscard]] bool require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "AMap reverse-geocoder contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    MapProviderPreferences preferences(
        root.filePath(QStringLiteral("preferences.ini")),
        std::make_unique<FakeSecretStore>()
    );
    FakeNetwork network;
    auto provider = makeAmapLibraryReverseGeocoder(
        &preferences,
        &network,
        QUrl(QStringLiteral("https://amap.test.invalid/v3/geocode/regeo"))
    );
    bool passed = require(!provider->available(), "provider started without authorization");
    passed &= require(
        preferences.storeAmapWebServiceKey(QStringLiteral("1234567890abcdef1234567890abcdef")),
        "Web Service key could not be stored"
    );
    passed &= require(!provider->available(), "storing a key implicitly enabled requests");
    preferences.setAmapReverseGeocodingAllowed(true);
    passed &= require(provider->available(), "explicit permission was not observed");

    bool completed = false;
    std::optional<BackendLibraryPlaceResolutionResult> result;
    QString error;
    provider->reverseGeocode(
        BackendLibraryPlaceResolutionCandidate{
            .latitude_e7 = 399'088'230,
            .longitude_e7 = 1'163'974'700,
            .photo_count = 1,
        },
        [&](std::optional<BackendLibraryPlaceResolutionResult> value, QString diagnostic) {
            completed = true;
            result = std::move(value);
            error = std::move(diagnostic);
        }
    );
    passed &= require(waitUntil([&]() { return completed; }), "request did not complete");
    const QUrlQuery query(network.last_url);
    const QString location = query.queryItemValue(QStringLiteral("location"));
    passed &= require(
        network.request_count == 1 && location.startsWith(QStringLiteral("116.403"))
            && location.contains(QStringLiteral(",39.910")),
        "request did not adapt WGS84 to the provider coordinate"
    );
    passed &= require(error.isEmpty() && result.has_value(), "valid response did not resolve");
    if (result) {
        passed &= require(
            result->country_code == QStringLiteral("CN")
                && result->administrative_area == QStringLiteral("北京市")
                && result->locality == QStringLiteral("北京市")
                && result->provider_id == QStringLiteral("amap-geocoding")
                && result->provider_version == QStringLiteral("v3"),
            "structured provider projection changed"
        );
    }

    completed = false;
    result.reset();
    error.clear();
    provider->reverseGeocode(
        BackendLibraryPlaceResolutionCandidate{
            .latitude_e7 = 488'566'000,
            .longitude_e7 = 23'522'000,
            .photo_count = 1,
        },
        [&](std::optional<BackendLibraryPlaceResolutionResult> value, QString diagnostic) {
            completed = true;
            result = std::move(value);
            error = std::move(diagnostic);
        }
    );
    passed &= require(
        completed && !result && network.request_count == 1 && error.contains("outside"),
        "coordinates outside the domestic route contacted AMap"
    );
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
