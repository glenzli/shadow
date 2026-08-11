#include "amap_place_search_service.hpp"

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
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
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

  protected:
    QNetworkReply*
    createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        ++request_count;
        last_url = request.url();
        return new FakeReply(
            request,
            QByteArrayLiteral(R"json({
              "status":"1","info":"OK","infocode":"10000","count":"1",
              "pois":[{
                "id":"B000A83VHF","name":"天安门","address":"东长安街",
                "pname":"北京市","cityname":"北京市",
                "location":"116.403714,39.910226"
              }]
            })json"),
            this
        );
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
        std::cerr << "AMap place-search service contract failed: " << message << '\n';
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
    AmapPlaceSearchService service(
        &preferences,
        &network,
        QUrl(QStringLiteral("https://amap.test.invalid/v3/place/text"))
    );

    service.search(QStringLiteral("天安门"), 39.908823, 116.397470);
    bool passed = require(
        network.request_count == 0 && !service.errorText().isEmpty(),
        "search contacted AMap before explicit permission"
    );
    passed &= require(
        preferences.storeAmapWebServiceKey(QStringLiteral("1234567890abcdef1234567890abcdef")),
        "Web Service key could not be stored"
    );
    preferences.setAmapPlacesAllowed(true);
    service.search(QStringLiteral("天安门"), 39.908823, 116.397470);
    passed &= require(
        waitUntil([&]() { return !service.busy(); }),
        "authorized place search did not complete"
    );
    const QUrlQuery query(network.last_url);
    passed &= require(
        network.request_count == 1
            && query.queryItemValue(QStringLiteral("keywords")) == QStringLiteral("天安门")
            && query.queryItemValue(QStringLiteral("location"))
                   .startsWith(QStringLiteral("116.403")),
        "request did not carry the query and adapted map center"
    );
    passed &= require(
        service.results().size() == 1 && service.errorText().isEmpty(),
        "valid response did not become one visible result"
    );
    if (!service.results().isEmpty()) {
        const QVariantMap result = service.results().constFirst().toMap();
        passed &= require(
            result.value(QStringLiteral("name")).toString() == QStringLiteral("天安门")
                && std::abs(result.value(QStringLiteral("latitude")).toDouble() - 39.908823)
                       < 0.000002
                && std::abs(result.value(QStringLiteral("longitude")).toDouble() - 116.397470)
                       < 0.000002,
            "provider result did not return to Shadow's WGS84 map coordinate"
        );
    }
    preferences.setAmapPlacesAllowed(false);
    passed &= require(
        !service.available() && service.results().isEmpty(),
        "revoking permission retained provider search state"
    );
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
