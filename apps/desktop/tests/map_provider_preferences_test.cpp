#include "map_provider_preferences.hpp"

#include <QFile>
#include <QSettings>
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
    QString value;
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

    [[nodiscard]] SecretStoreResult read(const QString&, const QString&) const override {
        return {
            .status = state_->read_status,
            .value = state_->value,
            .diagnostic = state_->diagnostic,
        };
    }

    [[nodiscard]] SecretStoreResult
    write(const QString& service, const QString& account, const QString& value) override {
        ++state_->write_count;
        if (service != QStringLiteral("dev.shadow.photo.maps")
            || account != QStringLiteral("google-maps-platform-api-key")) {
            return {
                .status = SecretStoreStatus::Failure,
                .diagnostic = QStringLiteral("unexpected secret identity"),
            };
        }
        if (state_->write_status == SecretStoreStatus::Success) {
            state_->value = value;
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
            || account != QStringLiteral("google-maps-platform-api-key")) {
            return {
                .status = SecretStoreStatus::Failure,
                .diagnostic = QStringLiteral("unexpected secret identity"),
            };
        }
        if (state_->remove_status == SecretStoreStatus::Success
            || state_->remove_status == SecretStoreStatus::NotFound) {
            state_->value.clear();
            state_->read_status = SecretStoreStatus::NotFound;
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
            )) {
            return EXIT_FAILURE;
        }

        preferences.setGoogleMapTilesAllowed(true);
        preferences.setGooglePlacesAllowed(true);
        preferences.setGoogleReverseGeocodingAllowed(true);
        preferences.setGoogleMapType(QStringLiteral("terrain"));
        if (!require(
                preferences.googleMapTilesAllowed() && preferences.googlePlacesAllowed()
                    && preferences.googleReverseGeocodingAllowed()
                    && preferences.libraryMapProvider() == QStringLiteral("google")
                    && preferences.googleMapType() == QStringLiteral("terrain"),
                "a stored key plus explicit tile permission admits the Google basemap"
            )) {
            return EXIT_FAILURE;
        }

        const SecretStoreResult native_read = preferences.readGoogleApiKey();
        if (!require(
                native_read.succeeded() && native_read.value == api_key,
                "native services can resolve the key without a QML property"
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
                && settings_bytes.contains("map_tiles_allowed=true")
                && settings_bytes.contains("places_allowed=true")
                && settings_bytes.contains("reverse_geocoding_allowed=true")
                && !settings_bytes.contains("provider=")
                && settings_bytes.contains("map_type=terrain"),
            "only non-secret active permissions and style are written to ordinary settings"
        )) {
        return EXIT_FAILURE;
    }

    {
        MapProviderPreferences reopened(settings_path, fakeStore(state));
        if (!require(
                reopened.googleApiKeyStored() && reopened.googleMapTilesAllowed()
                    && reopened.googlePlacesAllowed() && reopened.googleReverseGeocodingAllowed()
                    && reopened.libraryMapProvider() == QStringLiteral("google")
                    && reopened.googleMapType() == QStringLiteral("terrain"),
                "stored-key presence, permissions, provider, and style survive reconstruction"
            )
            || !require(
                reopened.removeGoogleApiKey() && !reopened.googleApiKeyStored()
                    && !reopened.googleMapTilesAllowed() && !reopened.googlePlacesAllowed()
                    && !reopened.googleReverseGeocodingAllowed()
                    && reopened.libraryMapProvider() == QStringLiteral("none")
                    && state->remove_count == 1,
                "removing a key revokes permissions and leaves no active basemap"
            )) {
            return EXIT_FAILURE;
        }
    }

    {
        MapProviderPreferences reopened(settings_path, fakeStore(state));
        reopened.setGooglePlacesAllowed(true);
        reopened.setGoogleMapTilesAllowed(true);
        if (!require(
                !reopened.googlePlacesAllowed() && !reopened.googleMapTilesAllowed()
                    && reopened.libraryMapProvider() == QStringLiteral("none")
                    && reopened.statusCode() == QStringLiteral("api-key-required"),
                "Google services cannot be enabled without a stored key"
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
