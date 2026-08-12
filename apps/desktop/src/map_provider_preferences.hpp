#pragma once

#include "secure_secret_store.hpp"

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

struct AmapJsCredentialsResult final {
    SecretStoreStatus status = SecretStoreStatus::Failure;
    QString api_key;
    QString security_code;
    QString diagnostic;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == SecretStoreStatus::Success;
    }
};

/// Owns optional external map-service authorization and the persisted Library
/// map-provider policy. `auto` is a policy rather than an effective provider;
/// the interactive map controller resolves it for one sticky map context.
///
/// Service permissions are ordinary preferences. Provider credentials stay
/// behind SecretStore and deliberately have no Q_PROPERTY or Q_INVOKABLE read
/// path, so settings QML can save or remove them but cannot retrieve them.
class MapProviderPreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool secureStorageAvailable READ secureStorageAvailable CONSTANT)
    Q_PROPERTY(bool googleApiKeyStored READ googleApiKeyStored NOTIFY googleApiKeyStoredChanged)
    Q_PROPERTY(
        bool amapWebServiceKeyStored READ amapWebServiceKeyStored NOTIFY
            amapWebServiceKeyStoredChanged
    )
    Q_PROPERTY(
        bool amapJsCredentialsStored READ amapJsCredentialsStored NOTIFY
            amapJsCredentialsStoredChanged
    )
    Q_PROPERTY(
        bool googlePlacesAllowed READ googlePlacesAllowed WRITE setGooglePlacesAllowed NOTIFY
            googlePlacesAllowedChanged
    )
    Q_PROPERTY(
        bool googleReverseGeocodingAllowed READ googleReverseGeocodingAllowed WRITE
            setGoogleReverseGeocodingAllowed NOTIFY googleReverseGeocodingAllowedChanged
    )
    Q_PROPERTY(
        bool amapPlacesAllowed READ amapPlacesAllowed WRITE setAmapPlacesAllowed NOTIFY
            amapPlacesAllowedChanged
    )
    Q_PROPERTY(
        bool amapReverseGeocodingAllowed READ amapReverseGeocodingAllowed WRITE
            setAmapReverseGeocodingAllowed NOTIFY amapReverseGeocodingAllowedChanged
    )
    Q_PROPERTY(
        QString libraryMapProvider READ libraryMapProvider WRITE setLibraryMapProvider NOTIFY
            libraryMapProviderChanged
    )
    Q_PROPERTY(QString mapStyle READ mapStyle WRITE setMapStyle NOTIFY mapStyleChanged)
    Q_PROPERTY(QString statusCode READ statusCode NOTIFY statusChanged)
    Q_PROPERTY(QString diagnosticText READ diagnosticText NOTIFY statusChanged)

  public:
    explicit MapProviderPreferences(
        const QString& isolated_settings_file,
        std::unique_ptr<SecretStore> secret_store,
        QObject* parent = nullptr
    );
    ~MapProviderPreferences() override;

    MapProviderPreferences(const MapProviderPreferences&) = delete;
    MapProviderPreferences& operator=(const MapProviderPreferences&) = delete;
    MapProviderPreferences(MapProviderPreferences&&) = delete;
    MapProviderPreferences& operator=(MapProviderPreferences&&) = delete;

    [[nodiscard]] bool secureStorageAvailable() const noexcept;
    [[nodiscard]] bool googleApiKeyStored() const noexcept;
    [[nodiscard]] bool amapWebServiceKeyStored() const noexcept;
    [[nodiscard]] bool amapJsCredentialsStored() const noexcept;
    [[nodiscard]] bool googlePlacesAllowed() const noexcept;
    [[nodiscard]] bool googleReverseGeocodingAllowed() const noexcept;
    [[nodiscard]] bool amapPlacesAllowed() const noexcept;
    [[nodiscard]] bool amapReverseGeocodingAllowed() const noexcept;
    [[nodiscard]] QString libraryMapProvider() const;
    [[nodiscard]] QString mapStyle() const;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] QString diagnosticText() const;

    void setLibraryMapProvider(const QString& provider);
    void setGooglePlacesAllowed(bool allowed);
    void setGoogleReverseGeocodingAllowed(bool allowed);
    void setAmapPlacesAllowed(bool allowed);
    void setAmapReverseGeocodingAllowed(bool allowed);
    void setMapStyle(const QString& map_style);

    Q_INVOKABLE bool storeGoogleApiKey(const QString& api_key);
    Q_INVOKABLE bool removeGoogleApiKey();
    Q_INVOKABLE bool storeAmapWebServiceKey(const QString& api_key);
    Q_INVOKABLE bool removeAmapWebServiceKey();
    Q_INVOKABLE bool storeAmapJsCredentials(const QString& api_key, const QString& security_code);
    Q_INVOKABLE bool removeAmapJsCredentials();
    Q_INVOKABLE void clearStatus();

    /// Native map services may request the key at the moment of an explicit
    /// network operation. This method is intentionally not exposed to QML.
    [[nodiscard]] SecretStoreResult readGoogleApiKey() const;
    [[nodiscard]] SecretStoreResult readAmapWebServiceKey() const;
    [[nodiscard]] AmapJsCredentialsResult readAmapJsCredentials() const;

  signals:
    void googleApiKeyStoredChanged();
    void amapWebServiceKeyStoredChanged();
    void amapJsCredentialsStoredChanged();
    void googlePlacesAllowedChanged();
    void googleReverseGeocodingAllowedChanged();
    void amapPlacesAllowedChanged();
    void amapReverseGeocodingAllowedChanged();
    void libraryMapProviderChanged();
    void mapStyleChanged();
    void statusChanged();

  private:
    [[nodiscard]] static bool validGoogleApiKey(const QString& api_key);
    [[nodiscard]] static bool validAmapCredential(const QString& credential);
    void loadCredentialState();
    void recoverSoleAvailableLibraryMapProvider();
    void disableGooglePermissions(bool persist);
    void disableAmapPermissions(bool persist);
    void persistPermission(const char* key, bool allowed);
    void setStatus(const QString& code, const QString& diagnostic = {});

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<SecretStore> secret_store_;
    bool google_api_key_stored_ = false;
    bool amap_web_service_key_stored_ = false;
    bool amap_js_credentials_stored_ = false;
    bool google_places_allowed_ = false;
    bool google_reverse_geocoding_allowed_ = false;
    bool amap_places_allowed_ = false;
    bool amap_reverse_geocoding_allowed_ = false;
    QString library_map_provider_ = QStringLiteral("none");
    QString map_style_ = QStringLiteral("roadmap");
    QString status_code_;
    QString diagnostic_text_;
    bool library_map_provider_preference_present_ = false;
};
