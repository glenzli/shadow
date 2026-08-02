#pragma once

#include "secure_secret_store.hpp"

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

/// Owns optional external map-service authorization.
///
/// Service permissions are ordinary preferences. The Google API key is kept
/// behind SecretStore and deliberately has no Q_PROPERTY or Q_INVOKABLE read
/// path, so settings QML can save or remove it but cannot retrieve it.
class MapProviderPreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool secureStorageAvailable READ secureStorageAvailable CONSTANT)
    Q_PROPERTY(bool googleApiKeyStored READ googleApiKeyStored NOTIFY googleApiKeyStoredChanged)
    Q_PROPERTY(
        bool googleMapTilesAllowed READ googleMapTilesAllowed WRITE setGoogleMapTilesAllowed NOTIFY
            googleMapTilesAllowedChanged
    )
    Q_PROPERTY(
        bool googlePlacesAllowed READ googlePlacesAllowed WRITE setGooglePlacesAllowed NOTIFY
            googlePlacesAllowedChanged
    )
    Q_PROPERTY(
        bool googleReverseGeocodingAllowed READ googleReverseGeocodingAllowed WRITE
            setGoogleReverseGeocodingAllowed NOTIFY googleReverseGeocodingAllowedChanged
    )
    Q_PROPERTY(QString libraryMapProvider READ libraryMapProvider NOTIFY libraryMapProviderChanged)
    Q_PROPERTY(
        QString googleMapType READ googleMapType WRITE setGoogleMapType NOTIFY googleMapTypeChanged
    )
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
    [[nodiscard]] bool googleMapTilesAllowed() const noexcept;
    [[nodiscard]] bool googlePlacesAllowed() const noexcept;
    [[nodiscard]] bool googleReverseGeocodingAllowed() const noexcept;
    [[nodiscard]] QString libraryMapProvider() const;
    [[nodiscard]] QString googleMapType() const;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] QString diagnosticText() const;

    void setGoogleMapTilesAllowed(bool allowed);
    void setGooglePlacesAllowed(bool allowed);
    void setGoogleReverseGeocodingAllowed(bool allowed);
    void setGoogleMapType(const QString& map_type);

    Q_INVOKABLE bool storeGoogleApiKey(const QString& api_key);
    Q_INVOKABLE bool removeGoogleApiKey();
    Q_INVOKABLE void clearStatus();

    /// Native map services may request the key at the moment of an explicit
    /// network operation. This method is intentionally not exposed to QML.
    [[nodiscard]] SecretStoreResult readGoogleApiKey() const;

  signals:
    void googleApiKeyStoredChanged();
    void googleMapTilesAllowedChanged();
    void googlePlacesAllowedChanged();
    void googleReverseGeocodingAllowedChanged();
    void libraryMapProviderChanged();
    void googleMapTypeChanged();
    void statusChanged();

  private:
    [[nodiscard]] static bool validGoogleApiKey(const QString& api_key);
    void loadKeyState();
    void disableAllPermissions(bool persist);
    void persistPermission(const char* key, bool allowed);
    void setStatus(const QString& code, const QString& diagnostic = {});

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<SecretStore> secret_store_;
    bool google_api_key_stored_ = false;
    bool google_map_tiles_allowed_ = false;
    bool google_places_allowed_ = false;
    bool google_reverse_geocoding_allowed_ = false;
    QString google_map_type_;
    QString status_code_;
    QString diagnostic_text_;
};
