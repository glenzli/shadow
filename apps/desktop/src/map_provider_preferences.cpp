#include "map_provider_preferences.hpp"

#include <QSettings>

#include <utility>

namespace {

constexpr auto google_secret_service = "dev.shadow.photo.maps";
constexpr auto google_secret_account = "google-maps-platform-api-key";
constexpr auto amap_web_secret_account = "amap-web-service-api-key";
constexpr auto amap_js_secret_account = "amap-js-api-credentials";
constexpr auto legacy_google_map_tiles_key = "maps/google/map_tiles_allowed";
constexpr auto google_places_key = "maps/google/places_allowed";
constexpr auto google_reverse_geocoding_key = "maps/google/reverse_geocoding_allowed";
constexpr auto legacy_google_map_type_key = "maps/google/map_type";
constexpr auto library_map_provider_key = "maps/library/provider";
constexpr auto library_map_style_key = "maps/library/style";
constexpr auto amap_places_key = "maps/amap/places_allowed";
constexpr auto amap_reverse_geocoding_key = "maps/amap/reverse_geocoding_allowed";
constexpr QChar amap_js_separator{0x001f};

[[nodiscard]] QString normalizedLibraryMapProvider(const QString& provider) {
    return provider == QStringLiteral("auto") || provider == QStringLiteral("google")
                   || provider == QStringLiteral("amap")
               ? provider
               : QStringLiteral("none");
}

[[nodiscard]] QString normalizedMapStyle(const QString& map_style) {
    return map_style == QStringLiteral("satellite") ? map_style : QStringLiteral("roadmap");
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
    google_places_allowed_(
        settings_->value(QString::fromLatin1(google_places_key), false).toBool()
    ),
    google_reverse_geocoding_allowed_(
        settings_->value(QString::fromLatin1(google_reverse_geocoding_key), false).toBool()
    ),
    amap_places_allowed_(settings_->value(QString::fromLatin1(amap_places_key), false).toBool()),
    amap_reverse_geocoding_allowed_(
        settings_->value(QString::fromLatin1(amap_reverse_geocoding_key), false).toBool()
    ),
    library_map_provider_(normalizedLibraryMapProvider(
        settings_
            ->value(
                QString::fromLatin1(library_map_provider_key),
                settings_->value(QString::fromLatin1(legacy_google_map_tiles_key), false).toBool()
                    ? QStringLiteral("google")
                    : QStringLiteral("none")
            )
            .toString()
    )),
    map_style_(normalizedMapStyle(settings_
                                      ->value(
                                          QString::fromLatin1(library_map_style_key),
                                          settings_->value(
                                              QString::fromLatin1(legacy_google_map_type_key),
                                              QStringLiteral("roadmap")
                                          )
                                      )
                                      .toString())) {
    library_map_provider_preference_present_ =
        settings_->contains(QString::fromLatin1(library_map_provider_key));
    // The pre-release native Google Tile route is intentionally retired. Migrate
    // its one useful choice into the provider-neutral WebView settings and do
    // not retain dormant tile/session preferences.
    if (settings_->contains(QString::fromLatin1(legacy_google_map_tiles_key))
        || settings_->contains(QString::fromLatin1(legacy_google_map_type_key))) {
        settings_->setValue(QString::fromLatin1(library_map_provider_key), library_map_provider_);
        settings_->setValue(QString::fromLatin1(library_map_style_key), map_style_);
        settings_->remove(QString::fromLatin1(legacy_google_map_tiles_key));
        settings_->remove(QString::fromLatin1(legacy_google_map_type_key));
        settings_->sync();
        library_map_provider_preference_present_ = true;
    }
    loadCredentialState();
    recoverSoleAvailableLibraryMapProvider();
}

MapProviderPreferences::~MapProviderPreferences() = default;

bool MapProviderPreferences::secureStorageAvailable() const noexcept {
    return secret_store_ != nullptr && secret_store_->available();
}

bool MapProviderPreferences::googleApiKeyStored() const noexcept {
    return google_api_key_stored_;
}

bool MapProviderPreferences::amapWebServiceKeyStored() const noexcept {
    return amap_web_service_key_stored_;
}

bool MapProviderPreferences::amapJsCredentialsStored() const noexcept {
    return amap_js_credentials_stored_;
}

bool MapProviderPreferences::googlePlacesAllowed() const noexcept {
    return google_places_allowed_;
}

bool MapProviderPreferences::googleReverseGeocodingAllowed() const noexcept {
    return google_reverse_geocoding_allowed_;
}

bool MapProviderPreferences::amapPlacesAllowed() const noexcept {
    return amap_places_allowed_;
}

bool MapProviderPreferences::amapReverseGeocodingAllowed() const noexcept {
    return amap_reverse_geocoding_allowed_;
}

QString MapProviderPreferences::libraryMapProvider() const {
    return library_map_provider_;
}

QString MapProviderPreferences::mapStyle() const {
    return map_style_;
}

QString MapProviderPreferences::statusCode() const {
    return status_code_;
}

QString MapProviderPreferences::diagnosticText() const {
    return diagnostic_text_;
}

void MapProviderPreferences::setLibraryMapProvider(const QString& provider) {
    const QString normalized = normalizedLibraryMapProvider(provider);
    if (normalized == QStringLiteral("auto") && !google_api_key_stored_
        && !amap_js_credentials_stored_) {
        setStatus(QStringLiteral("map-provider-required"));
        return;
    }
    if (normalized == QStringLiteral("google") && !google_api_key_stored_) {
        setStatus(QStringLiteral("api-key-required"));
        return;
    }
    if (normalized == QStringLiteral("amap") && !amap_js_credentials_stored_) {
        setStatus(QStringLiteral("amap-js-credentials-required"));
        return;
    }
    if (library_map_provider_ == normalized)
        return;
    library_map_provider_ = normalized;
    settings_->setValue(QString::fromLatin1(library_map_provider_key), library_map_provider_);
    settings_->sync();
    library_map_provider_preference_present_ = true;
    emit libraryMapProviderChanged();
}

void MapProviderPreferences::setMapStyle(const QString& map_style) {
    const QString normalized = normalizedMapStyle(map_style);
    if (map_style_ == normalized)
        return;
    map_style_ = normalized;
    settings_->setValue(QString::fromLatin1(library_map_style_key), map_style_);
    settings_->sync();
    emit mapStyleChanged();
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

void MapProviderPreferences::setAmapPlacesAllowed(const bool allowed) {
    const bool normalized = allowed && amap_web_service_key_stored_;
    if (amap_places_allowed_ == normalized) {
        if (allowed && !amap_web_service_key_stored_) {
            setStatus(QStringLiteral("amap-web-key-required"));
        }
        return;
    }
    amap_places_allowed_ = normalized;
    persistPermission(amap_places_key, normalized);
    emit amapPlacesAllowedChanged();
    if (allowed && !normalized) {
        setStatus(QStringLiteral("amap-web-key-required"));
    }
}

void MapProviderPreferences::setAmapReverseGeocodingAllowed(const bool allowed) {
    const bool normalized = allowed && amap_web_service_key_stored_;
    if (amap_reverse_geocoding_allowed_ == normalized) {
        if (allowed && !amap_web_service_key_stored_) {
            setStatus(QStringLiteral("amap-web-key-required"));
        }
        return;
    }
    amap_reverse_geocoding_allowed_ = normalized;
    persistPermission(amap_reverse_geocoding_key, normalized);
    emit amapReverseGeocodingAllowedChanged();
    if (allowed && !normalized) {
        setStatus(QStringLiteral("amap-web-key-required"));
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
    const bool credential_was_stored = google_api_key_stored_;
    if (!credential_was_stored) {
        google_api_key_stored_ = true;
        emit googleApiKeyStoredChanged();
    }
    if (!credential_was_stored && library_map_provider_ == QStringLiteral("none")) {
        setLibraryMapProvider(QStringLiteral("auto"));
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

    disableGooglePermissions(true);
    if (google_api_key_stored_) {
        google_api_key_stored_ = false;
        emit googleApiKeyStoredChanged();
    }
    if (library_map_provider_ == QStringLiteral("auto") && !amap_js_credentials_stored_) {
        library_map_provider_ = QStringLiteral("none");
        settings_->setValue(QString::fromLatin1(library_map_provider_key), library_map_provider_);
        settings_->sync();
        emit libraryMapProviderChanged();
    }
    setStatus(QStringLiteral("api-key-removed"));
    return true;
}

bool MapProviderPreferences::storeAmapWebServiceKey(const QString& api_key) {
    const QString normalized = api_key.trimmed();
    if (!validAmapCredential(normalized)) {
        setStatus(QStringLiteral("invalid-amap-web-key"));
        return false;
    }
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult result = secret_store_->write(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(amap_web_secret_account),
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
    if (!amap_web_service_key_stored_) {
        amap_web_service_key_stored_ = true;
        emit amapWebServiceKeyStoredChanged();
    }
    setStatus(QStringLiteral("amap-web-key-saved"));
    return true;
}

bool MapProviderPreferences::removeAmapWebServiceKey() {
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult result = secret_store_->remove(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(amap_web_secret_account)
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
    disableAmapPermissions(true);
    if (amap_web_service_key_stored_) {
        amap_web_service_key_stored_ = false;
        emit amapWebServiceKeyStoredChanged();
    }
    setStatus(QStringLiteral("amap-web-key-removed"));
    return true;
}

bool MapProviderPreferences::storeAmapJsCredentials(
    const QString& api_key,
    const QString& security_code
) {
    const QString normalized_key = api_key.trimmed();
    const QString normalized_security_code = security_code.trimmed();
    if (!validAmapCredential(normalized_key) || !validAmapCredential(normalized_security_code)) {
        setStatus(QStringLiteral("invalid-amap-js-credentials"));
        return false;
    }
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult result = secret_store_->write(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(amap_js_secret_account),
        normalized_key + amap_js_separator + normalized_security_code
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
    const bool credentials_were_stored = amap_js_credentials_stored_;
    if (!credentials_were_stored) {
        amap_js_credentials_stored_ = true;
        emit amapJsCredentialsStoredChanged();
    }
    if (!credentials_were_stored && library_map_provider_ == QStringLiteral("none")) {
        setLibraryMapProvider(QStringLiteral("auto"));
    }
    setStatus(QStringLiteral("amap-js-credentials-saved"));
    return true;
}

bool MapProviderPreferences::removeAmapJsCredentials() {
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult result = secret_store_->remove(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(amap_js_secret_account)
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
    if (library_map_provider_ == QStringLiteral("amap")
        || (library_map_provider_ == QStringLiteral("auto") && !google_api_key_stored_)) {
        library_map_provider_ = QStringLiteral("none");
        settings_->setValue(QString::fromLatin1(library_map_provider_key), library_map_provider_);
        settings_->sync();
        emit libraryMapProviderChanged();
    }
    if (amap_js_credentials_stored_) {
        amap_js_credentials_stored_ = false;
        emit amapJsCredentialsStoredChanged();
    }
    setStatus(QStringLiteral("amap-js-credentials-removed"));
    return true;
}

void MapProviderPreferences::clearStatus() {
    setStatus({});
}

SecretStoreResult MapProviderPreferences::readGoogleApiKey() const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("Shadow's local credential file is unavailable."),
        };
    }
    return secret_store_->read(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(google_secret_account)
    );
}

SecretStoreResult MapProviderPreferences::readAmapWebServiceKey() const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("Shadow's local credential file is unavailable."),
        };
    }
    return secret_store_->read(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(amap_web_secret_account)
    );
}

AmapJsCredentialsResult MapProviderPreferences::readAmapJsCredentials() const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("Shadow's local credential file is unavailable."),
        };
    }
    const SecretStoreResult secret = secret_store_->read(
        QString::fromLatin1(google_secret_service),
        QString::fromLatin1(amap_js_secret_account)
    );
    if (!secret.succeeded()) {
        return {
            .status = secret.status,
            .diagnostic = secret.diagnostic,
        };
    }
    const qsizetype separator = secret.value.indexOf(amap_js_separator);
    if (separator <= 0 || separator >= secret.value.size() - 1) {
        return {
            .status = SecretStoreStatus::Failure,
            .diagnostic = QStringLiteral("Stored AMap JS credentials are invalid."),
        };
    }
    const QString api_key = secret.value.left(separator);
    const QString security_code = secret.value.mid(separator + 1);
    if (!validAmapCredential(api_key) || !validAmapCredential(security_code)) {
        return {
            .status = SecretStoreStatus::Failure,
            .diagnostic = QStringLiteral("Stored AMap JS credentials are invalid."),
        };
    }
    return {
        .status = SecretStoreStatus::Success,
        .api_key = api_key,
        .security_code = security_code,
    };
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

bool MapProviderPreferences::validAmapCredential(const QString& credential) {
    if (credential.size() < 16 || credential.size() > 256) {
        return false;
    }
    for (const QChar character : credential) {
        if (character.isSpace() || !character.isPrint() || character == amap_js_separator) {
            return false;
        }
    }
    return true;
}

void MapProviderPreferences::loadCredentialState() {
    if (!secureStorageAvailable()) {
        disableGooglePermissions(false);
        disableAmapPermissions(false);
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return;
    }
    const SecretStoreResult google = readGoogleApiKey();
    if (google.status == SecretStoreStatus::Success) {
        google_api_key_stored_ = validGoogleApiKey(google.value);
        if (!google_api_key_stored_) {
            disableGooglePermissions(true);
            setStatus(QStringLiteral("invalid-stored-api-key"));
        }
    } else if (google.status == SecretStoreStatus::NotFound) {
        disableGooglePermissions(true);
    } else {
        disableGooglePermissions(false);
        setStatus(
            google.status == SecretStoreStatus::Unavailable
                ? QStringLiteral("secure-storage-unavailable")
                : QStringLiteral("secret-store-failed"),
            google.diagnostic
        );
    }

    const SecretStoreResult amap_web = readAmapWebServiceKey();
    if (amap_web.status == SecretStoreStatus::Success) {
        amap_web_service_key_stored_ = validAmapCredential(amap_web.value);
        if (!amap_web_service_key_stored_) {
            disableAmapPermissions(true);
            setStatus(QStringLiteral("invalid-stored-amap-web-key"));
        }
    } else if (amap_web.status == SecretStoreStatus::NotFound) {
        disableAmapPermissions(true);
    } else {
        disableAmapPermissions(false);
        setStatus(
            amap_web.status == SecretStoreStatus::Unavailable
                ? QStringLiteral("secure-storage-unavailable")
                : QStringLiteral("secret-store-failed"),
            amap_web.diagnostic
        );
    }

    const AmapJsCredentialsResult amap_js = readAmapJsCredentials();
    amap_js_credentials_stored_ = amap_js.succeeded();
    if (!amap_js_credentials_stored_ && library_map_provider_ == QStringLiteral("amap")) {
        library_map_provider_ = QStringLiteral("none");
        settings_->setValue(QString::fromLatin1(library_map_provider_key), library_map_provider_);
        settings_->sync();
    }
    if (amap_js.status == SecretStoreStatus::Failure) {
        setStatus(QStringLiteral("secret-store-failed"), amap_js.diagnostic);
    }
}

void MapProviderPreferences::recoverSoleAvailableLibraryMapProvider() {
    if (library_map_provider_preference_present_
        || library_map_provider_ != QStringLiteral("none")) {
        return;
    }
    if (google_api_key_stored_ == amap_js_credentials_stored_) {
        return;
    }
    setLibraryMapProvider(QStringLiteral("auto"));
}

void MapProviderPreferences::disableGooglePermissions(const bool persist) {
    if (library_map_provider_ == QStringLiteral("google")) {
        library_map_provider_ = QStringLiteral("none");
        if (persist) {
            settings_->setValue(
                QString::fromLatin1(library_map_provider_key),
                library_map_provider_
            );
            settings_->sync();
        }
        emit libraryMapProviderChanged();
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
}

void MapProviderPreferences::disableAmapPermissions(const bool persist) {
    if (amap_places_allowed_) {
        amap_places_allowed_ = false;
        if (persist) {
            persistPermission(amap_places_key, false);
        }
        emit amapPlacesAllowedChanged();
    }
    if (amap_reverse_geocoding_allowed_) {
        amap_reverse_geocoding_allowed_ = false;
        if (persist) {
            persistPermission(amap_reverse_geocoding_key, false);
        }
        emit amapReverseGeocodingAllowedChanged();
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
