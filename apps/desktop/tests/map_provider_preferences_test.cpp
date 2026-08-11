#include "map_provider_preferences.hpp"

#include <QFile>
#include <QHash>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

struct FakeSecretState {
    bool available = true;
    SecretStoreStatus read_status = SecretStoreStatus::NotFound;
    SecretStoreStatus write_status = SecretStoreStatus::Success;
    SecretStoreStatus remove_status = SecretStoreStatus::Success;
    QHash<QString, QString> values;
    QString diagnostic;
    int write_count = 0;
    int remove_count = 0;
};

class FakeSecretStore final : public SecretStore {
  public:
    explicit FakeSecretStore(std::shared_ptr<FakeSecretState> state) : state_(std::move(state)) {}

    [[nodiscard]] bool available() const noexcept override {
        return state_->available;
    }

    [[nodiscard]] SecretStoreResult read(const QString&, const QString& account) const override {
        if (state_->read_status == SecretStoreStatus::Failure
            || state_->read_status == SecretStoreStatus::Unavailable) {
            return {
                .status = state_->read_status,
                .diagnostic = state_->diagnostic,
            };
        }
        const auto found = state_->values.constFind(account);
        if (found == state_->values.cend()) {
            return {.status = SecretStoreStatus::NotFound};
        }
        return {
            .status = SecretStoreStatus::Success,
            .value = found.value(),
            .diagnostic = state_->diagnostic,
        };
    }

    [[nodiscard]] SecretStoreResult
    write(const QString& service, const QString& account, const QString& value) override {
        ++state_->write_count;
        const QStringList accepted_accounts{
            QStringLiteral("google-maps-platform-api-key"),
            QStringLiteral("amap-web-service-api-key"),
            QStringLiteral("amap-js-api-credentials"),
        };
        if (service != QStringLiteral("dev.shadow.photo.maps")
            || !accepted_accounts.contains(account)) {
            return {
                .status = SecretStoreStatus::Failure,
                .diagnostic = QStringLiteral("unexpected secret identity"),
            };
        }
        if (state_->write_status == SecretStoreStatus::Success) {
            state_->values.insert(account, value);
            state_->read_status = SecretStoreStatus::Success;
        }
        return {
            .status = state_->write_status,
            .diagnostic = state_->diagnostic,
        };
    }

    [[nodiscard]] SecretStoreResult
    remove(const QString& service, const QString& account) override {
        ++state_->remove_count;
        if (service != QStringLiteral("dev.shadow.photo.maps")
            || (account != QStringLiteral("google-maps-platform-api-key")
                && account != QStringLiteral("amap-web-service-api-key")
                && account != QStringLiteral("amap-js-api-credentials"))) {
            return {
                .status = SecretStoreStatus::Failure,
                .diagnostic = QStringLiteral("unexpected secret identity"),
            };
        }
        if (state_->remove_status == SecretStoreStatus::Success
            || state_->remove_status == SecretStoreStatus::NotFound) {
            state_->values.remove(account);
        }
        return {
            .status = state_->remove_status,
            .diagnostic = state_->diagnostic,
        };
    }

  private:
    std::shared_ptr<FakeSecretState> state_;
};

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Map-provider preferences contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::unique_ptr<SecretStore>
fakeStore(const std::shared_ptr<FakeSecretState>& state) {
    return std::make_unique<FakeSecretStore>(state);
}

} // namespace

int main() {
    QTemporaryDir root;
    if (!root.isValid()) {
        return EXIT_FAILURE;
    }
    const QString settings_path = root.filePath(QStringLiteral("preferences.ini"));
    const QString api_key = QStringLiteral("AIzaShadowDedicatedTestKey_1234567890");
    const QString amap_web_key = QStringLiteral("1234567890abcdef1234567890abcdef");
    const QString amap_js_key = QStringLiteral("abcdef1234567890abcdef1234567890");
    const QString amap_security_code = QStringLiteral("fedcba0987654321fedcba0987654321");
    auto state = std::make_shared<FakeSecretState>();

    {
        QSettings obsolete_settings(settings_path, QSettings::IniFormat);
        obsolete_settings.setValue(QStringLiteral("maps/library/provider"), QStringLiteral("osm"));
        obsolete_settings.sync();
    }

    {
        MapProviderPreferences preferences(settings_path, fakeStore(state));
        if (!require(preferences.secureStorageAvailable(), "fake storage is available")
            || !require(
                !preferences.googleApiKeyStored(),
                "an absent secret starts with no stored-key projection"
            )
            || !require(
                !preferences.storeGoogleApiKey(QStringLiteral("too-short"))
                    && preferences.statusCode() == QStringLiteral("invalid-api-key")
                    && state->write_count == 0,
                "invalid values never reach the credential store"
            )
            || !require(
                preferences.storeGoogleApiKey(api_key) && preferences.googleApiKeyStored()
                    && preferences.statusCode() == QStringLiteral("api-key-saved")
                    && state->write_count == 1,
                "a valid key is stored behind the native secret boundary"
            )
            || !require(
                preferences.storeAmapWebServiceKey(amap_web_key)
                    && preferences.amapWebServiceKeyStored()
                    && preferences.statusCode() == QStringLiteral("amap-web-key-saved"),
                "a valid AMap Web Service key is stored behind the same native boundary"
            )
            || !require(
                preferences.storeAmapJsCredentials(amap_js_key, amap_security_code)
                    && preferences.amapJsCredentialsStored()
                    && preferences.statusCode() == QStringLiteral("amap-js-credentials-saved"),
                "AMap JS key and security code are stored as one credential pair"
            )) {
            return EXIT_FAILURE;
        }

        preferences.setLibraryMapProvider(QStringLiteral("google"));
        preferences.setGooglePlacesAllowed(true);
        preferences.setGoogleReverseGeocodingAllowed(true);
        preferences.setAmapPlacesAllowed(true);
        preferences.setAmapReverseGeocodingAllowed(true);
        preferences.setMapStyle(QStringLiteral("satellite"));
        if (!require(
                preferences.googlePlacesAllowed() && preferences.googleReverseGeocodingAllowed()
                    && preferences.amapPlacesAllowed()
                    && preferences.amapReverseGeocodingAllowed()
                    && preferences.libraryMapProvider() == QStringLiteral("google")
                    && preferences.mapStyle() == QStringLiteral("satellite"),
                "a stored key plus explicit provider selection admits the Google Web map"
            )) {
            return EXIT_FAILURE;
        }

        const SecretStoreResult native_read = preferences.readGoogleApiKey();
        const SecretStoreResult amap_web_read = preferences.readAmapWebServiceKey();
        const AmapJsCredentialsResult amap_js_read = preferences.readAmapJsCredentials();
        if (!require(
                native_read.succeeded() && native_read.value == api_key,
                "native services can resolve the key without a QML property"
            )
            || !require(
                amap_web_read.succeeded() && amap_web_read.value == amap_web_key
                    && amap_js_read.succeeded() && amap_js_read.api_key == amap_js_key
                    && amap_js_read.security_code == amap_security_code,
                "native AMap services can resolve each credential without a QML read path"
            )) {
            return EXIT_FAILURE;
        }
    }

    QFile settings_file(settings_path);
    if (!require(
            settings_file.open(QIODevice::ReadOnly),
            "the isolated settings file is readable"
        )) {
        return EXIT_FAILURE;
    }
    const QByteArray settings_bytes = settings_file.readAll();
    if (!require(
            !settings_bytes.contains(api_key.toUtf8())
                && !settings_bytes.contains(amap_web_key.toUtf8())
                && !settings_bytes.contains(amap_js_key.toUtf8())
                && !settings_bytes.contains(amap_security_code.toUtf8())
                && !settings_bytes.contains("map_tiles_allowed")
                && settings_bytes.contains("places_allowed=true")
                && settings_bytes.contains("reverse_geocoding_allowed=true")
                && settings_bytes.contains("amap\\places_allowed=true")
                && settings_bytes.contains("amap\\reverse_geocoding_allowed=true")
                && settings_bytes.contains("library\\provider=google")
                && settings_bytes.contains("library\\style=satellite")
                && !settings_bytes.contains("map_type"),
            "only non-secret active permissions and style are written to ordinary settings"
        )) {
        return EXIT_FAILURE;
    }

    {
        MapProviderPreferences reopened(settings_path, fakeStore(state));
        if (!require(
                reopened.googleApiKeyStored() && reopened.googlePlacesAllowed()
                    && reopened.googleReverseGeocodingAllowed()
                    && reopened.amapWebServiceKeyStored()
                    && reopened.amapJsCredentialsStored() && reopened.amapPlacesAllowed()
                    && reopened.amapReverseGeocodingAllowed()
                    && reopened.libraryMapProvider() == QStringLiteral("google")
                    && reopened.mapStyle() == QStringLiteral("satellite"),
                "stored-key presence, permissions, provider, and style survive reconstruction"
            )
            || !require(
                reopened.removeAmapWebServiceKey() && !reopened.amapWebServiceKeyStored()
                    && !reopened.amapPlacesAllowed()
                    && !reopened.amapReverseGeocodingAllowed()
                    && reopened.amapJsCredentialsStored()
                    && reopened.googleApiKeyStored(),
                "removing the AMap Web key revokes only AMap Web-service permissions"
            )
            || !require(
                reopened.removeGoogleApiKey() && !reopened.googleApiKeyStored()
                    && !reopened.googlePlacesAllowed()
                    && !reopened.googleReverseGeocodingAllowed()
                    && reopened.libraryMapProvider() == QStringLiteral("none")
                    && state->remove_count == 2,
                "removing a key revokes permissions and leaves no active basemap"
            )) {
            return EXIT_FAILURE;
        }
    }

    {
        MapProviderPreferences reopened(settings_path, fakeStore(state));
        reopened.setGooglePlacesAllowed(true);
        reopened.setLibraryMapProvider(QStringLiteral("google"));
        reopened.setAmapPlacesAllowed(true);
        if (!require(
            !reopened.googlePlacesAllowed() && !reopened.amapPlacesAllowed()
                    && reopened.libraryMapProvider() == QStringLiteral("none")
                    && reopened.statusCode() == QStringLiteral("amap-web-key-required"),
                "provider services cannot be enabled without their stored key"
            )) {
            return EXIT_FAILURE;
        }
    }

    auto unavailable_state = std::make_shared<FakeSecretState>();
    unavailable_state->available = false;
    MapProviderPreferences unavailable(
        root.filePath(QStringLiteral("unavailable.ini")),
        fakeStore(unavailable_state)
    );
    if (!require(
            !unavailable.secureStorageAvailable()
                && unavailable.statusCode() == QStringLiteral("secure-storage-unavailable")
                && !unavailable.storeGoogleApiKey(api_key),
            "unsupported credential stores fail closed"
        )) {
        return EXIT_FAILURE;
    }

    auto failing_state = std::make_shared<FakeSecretState>();
    failing_state->read_status = SecretStoreStatus::Failure;
    failing_state->diagnostic = QStringLiteral("credential access denied");
    const QString failure_settings_path = root.filePath(QStringLiteral("failure.ini"));
    {
        QSettings settings(failure_settings_path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("maps/google/places_allowed"), true);
        settings.sync();
    }
    MapProviderPreferences failing(failure_settings_path, fakeStore(failing_state));
    QSettings retained_settings(failure_settings_path, QSettings::IniFormat);
    return require(
               !failing.googlePlacesAllowed()
                   && retained_settings.value(QStringLiteral("maps/google/places_allowed")).toBool()
                   && failing.statusCode() == QStringLiteral("secret-store-failed")
                   && failing.diagnosticText() == QStringLiteral("credential access denied"),
               "a transient credential failure fails closed without erasing the user's permission"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
