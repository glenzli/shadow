#include "map/library_web_map_controller.hpp"

#include "map_provider_preferences.hpp"

#include <QHash>
#include <QObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

class MemorySecretStore final : public SecretStore {
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
    [[nodiscard]] SecretStoreResult write(
        const QString&,
        const QString& account,
        const QString& value
    ) override {
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

class FakeWebView final : public QObject {
    Q_OBJECT

  public:
    Q_INVOKABLE void loadHtml(const QString& document) { html = document; }
    Q_INVOKABLE void runJavaScript(const QString& source) { scripts.push_back(source); }

    QString html;
    QStringList scripts;
};

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) std::cerr << "Library Web map controller contract failed: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    QTemporaryDir root;
    if (!root.isValid()) return EXIT_FAILURE;

    MapProviderPreferences preferences(
        root.filePath(QStringLiteral("map.ini")),
        std::make_unique<MemorySecretStore>()
    );
    const QString google_key = QStringLiteral("AIzaShadowDedicatedTestKey_1234567890");
    const QString amap_key = QStringLiteral("abcdef1234567890abcdef1234567890");
    const QString amap_security = QStringLiteral("fedcba0987654321fedcba0987654321");
    if (!preferences.storeGoogleApiKey(google_key)
        || !preferences.storeAmapJsCredentials(amap_key, amap_security)) {
        return EXIT_FAILURE;
    }

    LibraryWebMapController controller(&preferences);
    FakeWebView web_view;
    controller.attachWebView(&web_view);
    controller.setActive(true);
    preferences.setLibraryMapProvider(QStringLiteral("google"));

    bool valid = require(
        web_view.html.contains(QStringLiteral("maps.googleapis.com/maps/api/js"))
            && web_view.html.contains(google_key)
            && !web_view.html.contains(QStringLiteral("tile.googleapis.com"))
            && !web_view.html.contains(QStringLiteral("createSession")),
        "Google uses the official JavaScript map document without a native Tile session route"
    );
    valid &= require(
        controller.metaObject()->indexOfProperty("apiKey") < 0
            && controller.metaObject()->indexOfMethod("readGoogleApiKey()") < 0,
        "credentials have no QML-readable controller property or invokable"
    );

    const QVariantMap cluster{
        {QStringLiteral("latitude"), 31.2304},
        {QStringLiteral("longitude"), 121.4737},
        {QStringLiteral("photoCount"), 1},
        {QStringLiteral("photoId"), QStringLiteral("photo-1")},
        {QStringLiteral("title"), QStringLiteral("Shanghai")},
    };
    controller.setClusters({cluster});
    controller.consumeEvents(QStringLiteral("[{\"kind\":\"ready\"}]"));
    valid &= require(
        controller.ready() && !web_view.scripts.isEmpty()
            && web_view.scripts.constLast().contains(QStringLiteral("shadowMapApplyState")),
        "provider readiness flushes the current cluster state into the WebView"
    );

    QVariantMap activated;
    QObject::connect(
        &controller,
        &LibraryWebMapController::clusterActivated,
        [&activated](const QVariantMap& value) { activated = value; }
    );
    controller.consumeEvents(QStringLiteral("[{\"kind\":\"cluster\",\"index\":0}]"));
    valid &= require(
        activated.value(QStringLiteral("photoId")).toString() == QStringLiteral("photo-1"),
        "a WebView marker event resolves against the native cluster snapshot"
    );

    preferences.setLibraryMapProvider(QStringLiteral("amap"));
    valid &= require(
        web_view.html.contains(QStringLiteral("webapi.amap.com/maps"))
            && web_view.html.contains(amap_key) && web_view.html.contains(amap_security),
        "AMap uses the same WebView owner with its JS key and security code"
    );

    double proposed_latitude = 0.0;
    double proposed_longitude = 0.0;
    QObject::connect(
        &controller,
        &LibraryWebMapController::coordinateProposed,
        [&proposed_latitude, &proposed_longitude](const double latitude, const double longitude) {
            proposed_latitude = latitude;
            proposed_longitude = longitude;
        }
    );
    controller.consumeEvents(QStringLiteral(
        "[{\"kind\":\"placement\",\"latitude\":31.228457,\"longitude\":121.478223}]"
    ));
    valid &= require(
        proposed_latitude > 31.22 && proposed_latitude < 31.24
            && proposed_longitude > 121.46 && proposed_longitude < 121.48,
        "AMap events return through the WGS84 provider boundary"
    );

    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "library_web_map_controller_test.moc"
