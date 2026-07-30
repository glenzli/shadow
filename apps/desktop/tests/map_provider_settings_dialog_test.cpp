#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QString>
#include <QVariant>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeMapProviderPreferences final : public QObject {
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
    Q_PROPERTY(
        QString libraryMapProvider READ libraryMapProvider WRITE setLibraryMapProvider NOTIFY
            libraryMapProviderChanged
    )
    Q_PROPERTY(
        QString googleMapType READ googleMapType WRITE setGoogleMapType NOTIFY googleMapTypeChanged
    )
    Q_PROPERTY(QString statusCode READ statusCode NOTIFY statusChanged)
    Q_PROPERTY(QString diagnosticText READ diagnosticText NOTIFY statusChanged)

  public:
    [[nodiscard]] bool secureStorageAvailable() const noexcept {
        return true;
    }
    [[nodiscard]] bool googleApiKeyStored() const noexcept {
        return key_stored_;
    }
    [[nodiscard]] bool googleMapTilesAllowed() const noexcept {
        return map_tiles_allowed_;
    }
    [[nodiscard]] bool googlePlacesAllowed() const noexcept {
        return places_allowed_;
    }
    [[nodiscard]] bool googleReverseGeocodingAllowed() const noexcept {
        return reverse_geocoding_allowed_;
    }
    [[nodiscard]] QString statusCode() const {
        return status_code_;
    }
    [[nodiscard]] QString libraryMapProvider() const {
        return library_map_provider_;
    }
    [[nodiscard]] QString googleMapType() const {
        return google_map_type_;
    }
    [[nodiscard]] QString diagnosticText() const {
        return {};
    }

    void setGoogleMapTilesAllowed(const bool allowed) {
        map_tiles_allowed_ = allowed;
        if (!allowed) {
            setLibraryMapProvider(QStringLiteral("osm"));
        }
        emit googleMapTilesAllowedChanged();
    }
    void setGooglePlacesAllowed(const bool allowed) {
        places_allowed_ = allowed;
        emit googlePlacesAllowedChanged();
    }
    void setGoogleReverseGeocodingAllowed(const bool allowed) {
        reverse_geocoding_allowed_ = allowed;
        emit googleReverseGeocodingAllowedChanged();
    }
    void setLibraryMapProvider(const QString& provider) {
        const QString normalized =
            provider == QStringLiteral("google") && key_stored_ && map_tiles_allowed_
                ? QStringLiteral("google")
                : QStringLiteral("osm");
        if (library_map_provider_ == normalized) {
            return;
        }
        library_map_provider_ = normalized;
        emit libraryMapProviderChanged();
    }
    void setGoogleMapType(const QString& map_type) {
        if (google_map_type_ == map_type) {
            return;
        }
        google_map_type_ = map_type;
        emit googleMapTypeChanged();
    }

    Q_INVOKABLE bool storeGoogleApiKey(const QString& value) {
        ++save_count;
        saved_value = value;
        key_stored_ = true;
        status_code_ = QStringLiteral("api-key-saved");
        emit googleApiKeyStoredChanged();
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE bool removeGoogleApiKey() {
        ++remove_count;
        key_stored_ = false;
        map_tiles_allowed_ = false;
        places_allowed_ = false;
        reverse_geocoding_allowed_ = false;
        library_map_provider_ = QStringLiteral("osm");
        status_code_ = QStringLiteral("api-key-removed");
        emit googleApiKeyStoredChanged();
        emit googleMapTilesAllowedChanged();
        emit googlePlacesAllowedChanged();
        emit googleReverseGeocodingAllowedChanged();
        emit libraryMapProviderChanged();
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE void clearStatus() {
        status_code_.clear();
        emit statusChanged();
    }

    int save_count = 0;
    int remove_count = 0;
    QString saved_value;

  signals:
    void googleApiKeyStoredChanged();
    void googleMapTilesAllowedChanged();
    void googlePlacesAllowedChanged();
    void googleReverseGeocodingAllowedChanged();
    void libraryMapProviderChanged();
    void googleMapTypeChanged();
    void statusChanged();

  private:
    bool key_stored_ = false;
    bool map_tiles_allowed_ = false;
    bool places_allowed_ = false;
    bool reverse_geocoding_allowed_ = false;
    QString library_map_provider_ = QStringLiteral("osm");
    QString google_map_type_ = QStringLiteral("roadmap");
    QString status_code_;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Map-provider settings dialog contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool click(QObject* object) {
    const bool invoked = QMetaObject::invokeMethod(object, "clicked");
    drainBindings();
    return invoked;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.MapProviderSettingsContract"),
        QStringLiteral("MapProviderSettingsDialog")
    );

    FakeMapProviderPreferences preferences;
    std::unique_ptr<QObject> dialog{component.createWithInitialProperties({
        {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("hostWidth"), 1200.0},
        {QStringLiteral("hostHeight"), 800.0},
    })};
    if (!dialog) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QObject* const field = dialog->findChild<QObject*>(QStringLiteral("googleApiKeyField"));
    QObject* const save = dialog->findChild<QObject*>(QStringLiteral("googleApiKeySaveButton"));
    QObject* const remove = dialog->findChild<QObject*>(QStringLiteral("googleApiKeyRemoveButton"));
    QObject* const tiles =
        dialog->findChild<QObject*>(QStringLiteral("googleMapTilesPermissionSwitch"));
    QObject* const places =
        dialog->findChild<QObject*>(QStringLiteral("googlePlacesPermissionSwitch"));
    QObject* const reverse =
        dialog->findChild<QObject*>(QStringLiteral("googleReverseGeocodingPermissionSwitch"));
    QObject* const osm_provider =
        dialog->findChild<QObject*>(QStringLiteral("openStreetMapProviderButton"));
    QObject* const google_provider =
        dialog->findChild<QObject*>(QStringLiteral("googleMapsProviderButton"));
    QObject* const satellite_style =
        dialog->findChild<QObject*>(QStringLiteral("googleSatelliteStyleButton"));
    if (!require(
            field != nullptr && save != nullptr && remove != nullptr && tiles != nullptr
                && places != nullptr && reverse != nullptr && osm_provider != nullptr
                && google_provider != nullptr && satellite_style != nullptr,
            "the packaged dialog exposes its credential and permission controls"
        )
        || !require(
            !tiles->property("enabled").toBool() && !places->property("enabled").toBool()
                && !reverse->property("enabled").toBool(),
            "service permissions stay disabled before a key is stored"
        )) {
        return EXIT_FAILURE;
    }

    const QString api_key = QStringLiteral("AIzaDialogContractKey_1234567890");
    field->setProperty("text", api_key);
    QMetaObject::invokeMethod(dialog.get(), "closed");
    drainBindings();
    if (!require(
            field->property("text").toString().isEmpty(),
            "closing the panel drops any unsaved credential text"
        )) {
        return EXIT_FAILURE;
    }

    field->setProperty("text", api_key);
    drainBindings();
    if (!require(
            save->property("enabled").toBool() && click(save) && preferences.save_count == 1
                && preferences.saved_value == api_key
                && field->property("text").toString().isEmpty(),
            "saving delegates the entered secret once and immediately clears the field"
        )
        || !require(
            tiles->property("enabled").toBool() && places->property("enabled").toBool()
                && reverse->property("enabled").toBool()
                && !google_provider->property("enabled").toBool(),
            "stored-key projection unlocks each explicit service permission"
        )
        || !require(
            (preferences.setGoogleMapTilesAllowed(true), drainBindings(), true)
                && google_provider->property("enabled").toBool() && click(google_provider)
                && preferences.libraryMapProvider() == QStringLiteral("google")
                && osm_provider->property("selected").toBool() == false,
            "an allowed Google tile service can become the Library map source"
        )
        || !require(
            click(satellite_style) && preferences.googleMapType() == QStringLiteral("satellite"),
            "the selected Google map style is delegated to provider preferences"
        )
        || !require(
            click(remove) && preferences.remove_count == 1 && !tiles->property("enabled").toBool(),
            "removing the stored key returns the panel to its safe default"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "map_provider_settings_dialog_test.moc"
