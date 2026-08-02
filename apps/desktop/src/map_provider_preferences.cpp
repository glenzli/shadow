#include "map_provider_preferences.hpp"

#include <QSettings>

#include <utility>

namespace {

constexpr auto google_secret_service = "dev.shadow.photo.maps";
constexpr auto google_secret_account = "google-maps-platform-api-key";
constexpr auto google_map_tiles_key = "maps/google/map_tiles_allowed";
constexpr auto google_places_key = "maps/google/places_allowed";
constexpr auto google_reverse_geocoding_key = "maps/google/reverse_geocoding_allowed";
constexpr auto google_map_type_key = "maps/google/map_type";

[[nodiscard]] QString normalizedGoogleMapType(const QString& map_type) {
    if (map_type == QStringLiteral("satellite") || map_type == QStringLiteral("terrain")) {
        return map_type;
    }
    return QStringLiteral("roadmap");
}

[[nodiscard]] std::unique_ptr<QSettings> makeSettings(const QString& isolated_settings_file) {
    if (isolated_settings_file.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat);
}

} // namespace

MapProviderPreferences::MapProviderPreferences(
    const QString& isolated_settings_file,
    std::unique_ptr<SecretStore> secret_store,
    QObject* const parent
) :
    QObject(parent), settings_(makeSettings(isolated_settings_file)),
    secret_store_(std::move(secret_store)),
    google_map_tiles_allowed_(
        settings_->value(QString::fromLatin1(google_map_tiles_key), false).toBool()
    ),
    google_places_allowed_(
        settings_->value(QString::fromLatin1(google_places_key), false).toBool()
    ),
    google_reverse_geocoding_allowed_(
        settings_->value(QString::fromLatin1(google_reverse_geocoding_key), false).toBool()
    ),
    google_map_type_(normalizedGoogleMapType(
        settings_->value(QString::fromLatin1(google_map_type_key), QStringLiteral("roadmap"))
            .toString()
    )) {
    // Pre-release v1 keeps no dormant provider-selection state. OSM is no
    // longer a supported basemap, so discard the old preference outright.
    if (settings_->contains(QStringLiteral("maps/library/provider"))) {
        settings_->remove(QStringLiteral("maps/library/provider"));
        settings_->sync();
    }
    loadKeyState();
}

MapProviderPreferences::~MapProviderPreferences() = default;

bool MapProviderPreferences::secureStorageAvailable() const noexcept {
    return secret_store_ != nullptr && secret_store_->available();
}

bool MapProviderPreferences::googleApiKeyStored() const noexcept {
    return google_api_key_stored_;
}

bool MapProviderPreferences::googleMapTilesAllowed() const noexcept {
    return google_map_tiles_allowed_;
}

bool MapProviderPreferences::googlePlacesAllowed() const noexcept {
    return google_places_allowed_;
}

bool MapProviderPreferences::googleReverseGeocodingAllowed() const noexcept {
    return google_reverse_geocoding_allowed_;
}

QString MapProviderPreferences::libraryMapProvider() const {
    return google_api_key_stored_ && google_map_tiles_allowed_ ? QStringLiteral("google")
                                                               : QStringLiteral("none");
}

QString MapProviderPreferences::googleMapType() const {
    return google_map_type_;
}

QString MapProviderPreferences::statusCode() const {
    return status_code_;
}

QString MapProviderPreferences::diagnosticText() const {
    return diagnostic_text_;
}

void MapProviderPreferences::setGoogleMapTilesAllowed(const bool allowed) {
    const QString previous_provider = libraryMapProvider();
    const bool normalized = allowed && google_api_key_stored_;
    if (google_map_tiles_allowed_ == normalized) {
        if (allowed && !google_api_key_stored_) {
            setStatus(QStringLiteral("api-key-required"));
        }
        return;
    }
    google_map_tiles_allowed_ = normalized;
    persistPermission(google_map_tiles_key, normalized);
    emit googleMapTilesAllowedChanged();
    if (libraryMapProvider() != previous_provider) {
        emit libraryMapProviderChanged();
    }
    if (allowed && !normalized) {
        setStatus(QStringLiteral("api-key-required"));
    }
}

void MapProviderPreferences::setGoogleMapType(const QString& map_type) {
    const QString normalized = normalizedGoogleMapType(map_type);
    if (google_map_type_ == normalized) {
        return;
    }
    google_map_type_ = normalized;
    settings_->setValue(QString::fromLatin1(google_map_type_key), google_map_type_);
    settings_->sync();
    emit googleMapTypeChanged();
}

void MapProviderPreferences::setGooglePlacesAllowed(const bool allowed) {
    const bool normalized = allowed && google_api_key_stored_;
    if (google_places_allowed_ == normalized) {
        if (allowed && !google_api_key_stored_) {
            setStatus(QStringLiteral("api-key-required"));
        }
        return;
    }
    google_places_allowed_ = normalized;
    persistPermission(google_places_key, normalized);
    emit googlePlacesAllowedChanged();
    if (allowed && !normalized) {
        setStatus(QStringLiteral("api-key-required"));
    }
}

void MapProviderPreferences::setGoogleReverseGeocodingAllowed(const bool allowed) {
    const bool normalized = allowed && google_api_key_stored_;
    if (google_reverse_geocoding_allowed_ == normalized) {
        if (allowed && !google_api_key_stored_) {
            setStatus(QStringLiteral("api-key-required"));
        }
        return;
    }
    google_reverse_geocoding_allowed_ = normalized;
    persistPermission(google_reverse_geocoding_key, normalized);
    emit googleReverseGeocodingAllowedChanged();
    if (allowed && !normalized) {
        setStatus(QStringLiteral("api-key-required"));
    }
}

bool MapProviderPreferences::storeGoogleApiKey(const QString& api_key) {
    const QString normalized = api_key.trimmed();
    if (!validGoogleApiKey(normalized)) {
        setStatus(QStringLiteral("invalid-api-key"));
        return false;
    }
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }

    const SecretStoreResult result = secret_store_->write(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(google_secret_account),
        normalized
    );
    if (!result.succeeded()) {
        setStatus(
            result.status == SecretStoreStatus::Unavailable
                ? QStringLiteral("secure-storage-unavailable")
                : QStringLiteral("secret-store-failed"),
            result.diagnostic
        );
        return false;
    }
    if (!google_api_key_stored_) {
        google_api_key_stored_ = true;
        emit googleApiKeyStoredChanged();
    }
    setStatus(QStringLiteral("api-key-saved"));
    return true;
}

bool MapProviderPreferences::removeGoogleApiKey() {
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult result = secret_store_->remove(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(google_secret_account)
    );
    if (result.status != SecretStoreStatus::Success
        && result.status != SecretStoreStatus::NotFound) {
        setStatus(
            result.status == SecretStoreStatus::Unavailable
                ? QStringLiteral("secure-storage-unavailable")
                : QStringLiteral("secret-store-failed"),
            result.diagnostic
        );
        return false;
    }

    disableAllPermissions(true);
    if (google_api_key_stored_) {
        google_api_key_stored_ = false;
        emit googleApiKeyStoredChanged();
    }
    setStatus(QStringLiteral("api-key-removed"));
    return true;
}

void MapProviderPreferences::clearStatus() {
    setStatus({});
}

SecretStoreResult MapProviderPreferences::readGoogleApiKey() const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("The operating system credential store is unavailable."),
        };
    }
    return secret_store_->read(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(google_secret_account)
    );
}

bool MapProviderPreferences::validGoogleApiKey(const QString& api_key) {
    if (api_key.size() < 20 || api_key.size() > 512) {
        return false;
    }
    for (const QChar character : api_key) {
        if (character.isSpace() || !character.isPrint()) {
            return false;
        }
    }
    return true;
}

void MapProviderPreferences::loadKeyState() {
    if (!secureStorageAvailable()) {
        disableAllPermissions(false);
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return;
    }
    const SecretStoreResult result = readGoogleApiKey();
    if (result.status == SecretStoreStatus::Success) {
        google_api_key_stored_ = validGoogleApiKey(result.value);
        if (!google_api_key_stored_) {
            disableAllPermissions(true);
            setStatus(QStringLiteral("invalid-stored-api-key"));
        }
        return;
    }
    if (result.status == SecretStoreStatus::NotFound) {
        disableAllPermissions(true);
        return;
    }
    disableAllPermissions(false);
    setStatus(
        result.status == SecretStoreStatus::Unavailable
            ? QStringLiteral("secure-storage-unavailable")
            : QStringLiteral("secret-store-failed"),
        result.diagnostic
    );
}

void MapProviderPreferences::disableAllPermissions(const bool persist) {
    const QString previous_provider = libraryMapProvider();
    if (google_map_tiles_allowed_) {
        google_map_tiles_allowed_ = false;
        if (persist) {
            persistPermission(google_map_tiles_key, false);
        }
        emit googleMapTilesAllowedChanged();
    }
    if (google_places_allowed_) {
        google_places_allowed_ = false;
        if (persist) {
            persistPermission(google_places_key, false);
        }
        emit googlePlacesAllowedChanged();
    }
    if (google_reverse_geocoding_allowed_) {
        google_reverse_geocoding_allowed_ = false;
        if (persist) {
            persistPermission(google_reverse_geocoding_key, false);
        }
        emit googleReverseGeocodingAllowedChanged();
    }
    if (libraryMapProvider() != previous_provider) {
        emit libraryMapProviderChanged();
    }
}

void MapProviderPreferences::persistPermission(const char* const key, const bool allowed) {
    settings_->setValue(QString::fromLatin1(key), allowed);
    settings_->sync();
}

void MapProviderPreferences::setStatus(const QString& code, const QString& diagnostic) {
    if (status_code_ == code && diagnostic_text_ == diagnostic) {
        return;
    }
    status_code_ = code;
    diagnostic_text_ = diagnostic;
    emit statusChanged();
}
