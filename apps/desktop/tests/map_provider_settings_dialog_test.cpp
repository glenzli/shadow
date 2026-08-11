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
    [[nodiscard]] bool secureStorageAvailable() const noexcept {
        return true;
    }
    [[nodiscard]] bool googleApiKeyStored() const noexcept {
        return key_stored_;
    }
    [[nodiscard]] bool amapWebServiceKeyStored() const noexcept {
        return amap_web_key_stored_;
    }
    [[nodiscard]] bool amapJsCredentialsStored() const noexcept {
        return amap_js_credentials_stored_;
    }
    [[nodiscard]] bool googlePlacesAllowed() const noexcept {
        return places_allowed_;
    }
    [[nodiscard]] bool googleReverseGeocodingAllowed() const noexcept {
        return reverse_geocoding_allowed_;
    }
    [[nodiscard]] bool amapPlacesAllowed() const noexcept {
        return amap_places_allowed_;
    }
    [[nodiscard]] bool amapReverseGeocodingAllowed() const noexcept {
        return amap_reverse_geocoding_allowed_;
    }
    [[nodiscard]] QString statusCode() const {
        return status_code_;
    }
    [[nodiscard]] QString libraryMapProvider() const {
        return library_map_provider_;
    }
    [[nodiscard]] QString mapStyle() const {
        return map_style_;
    }
    [[nodiscard]] QString diagnosticText() const {
        return {};
    }

    void setLibraryMapProvider(const QString& provider) {
        if ((provider == QStringLiteral("google") && !key_stored_)
            || (provider == QStringLiteral("amap") && !amap_js_credentials_stored_))
            return;
        const QString normalized =
            provider == QStringLiteral("google") || provider == QStringLiteral("amap")
                ? provider
                : QStringLiteral("none");
        if (library_map_provider_ == normalized)
            return;
        library_map_provider_ = normalized;
        emit libraryMapProviderChanged();
    }
    void setGooglePlacesAllowed(const bool allowed) {
        places_allowed_ = allowed;
        emit googlePlacesAllowedChanged();
    }
    void setGoogleReverseGeocodingAllowed(const bool allowed) {
        reverse_geocoding_allowed_ = allowed;
        emit googleReverseGeocodingAllowedChanged();
    }
    void setAmapPlacesAllowed(const bool allowed) {
        amap_places_allowed_ = allowed;
        emit amapPlacesAllowedChanged();
    }
    void setAmapReverseGeocodingAllowed(const bool allowed) {
        amap_reverse_geocoding_allowed_ = allowed;
        emit amapReverseGeocodingAllowedChanged();
    }
    void setMapStyle(const QString& map_style) {
        if (map_style_ == map_style) {
            return;
        }
        map_style_ = map_style;
        emit mapStyleChanged();
    }

    Q_INVOKABLE bool storeGoogleApiKey(const QString& value) {
        ++save_count;
        saved_value = value;
        key_stored_ = true;
        status_code_ = QStringLiteral("api-key-saved");
        emit googleApiKeyStoredChanged();
        if (library_map_provider_ == QStringLiteral("none")) {
            setLibraryMapProvider(QStringLiteral("google"));
        }
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE bool removeGoogleApiKey() {
        ++remove_count;
        key_stored_ = false;
        if (library_map_provider_ == QStringLiteral("google")) {
            library_map_provider_ = QStringLiteral("none");
            emit libraryMapProviderChanged();
        }
        places_allowed_ = false;
        reverse_geocoding_allowed_ = false;
        status_code_ = QStringLiteral("api-key-removed");
        emit googleApiKeyStoredChanged();
        emit googlePlacesAllowedChanged();
        emit googleReverseGeocodingAllowedChanged();
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE bool storeAmapWebServiceKey(const QString& value) {
        ++amap_web_save_count;
        saved_amap_web_value = value;
        amap_web_key_stored_ = true;
        status_code_ = QStringLiteral("amap-web-key-saved");
        emit amapWebServiceKeyStoredChanged();
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE bool removeAmapWebServiceKey() {
        ++amap_web_remove_count;
        amap_web_key_stored_ = false;
        amap_places_allowed_ = false;
        amap_reverse_geocoding_allowed_ = false;
        status_code_ = QStringLiteral("amap-web-key-removed");
        emit amapWebServiceKeyStoredChanged();
        emit amapPlacesAllowedChanged();
        emit amapReverseGeocodingAllowedChanged();
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE bool storeAmapJsCredentials(const QString& api_key, const QString& security_code) {
        ++amap_js_save_count;
        saved_amap_js_key = api_key;
        saved_amap_security_code = security_code;
        amap_js_credentials_stored_ = true;
        status_code_ = QStringLiteral("amap-js-credentials-saved");
        emit amapJsCredentialsStoredChanged();
        if (library_map_provider_ == QStringLiteral("none")) {
            setLibraryMapProvider(QStringLiteral("amap"));
        }
        emit statusChanged();
        return true;
    }

    Q_INVOKABLE bool removeAmapJsCredentials() {
        if (library_map_provider_ == QStringLiteral("amap")) {
            library_map_provider_ = QStringLiteral("none");
            emit libraryMapProviderChanged();
        }
        amap_js_credentials_stored_ = false;
        status_code_ = QStringLiteral("amap-js-credentials-removed");
        emit amapJsCredentialsStoredChanged();
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
    int amap_web_save_count = 0;
    int amap_web_remove_count = 0;
    int amap_js_save_count = 0;
    QString saved_amap_web_value;
    QString saved_amap_js_key;
    QString saved_amap_security_code;

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
    bool key_stored_ = false;
    bool amap_web_key_stored_ = false;
    bool amap_js_credentials_stored_ = false;
    bool places_allowed_ = false;
    bool reverse_geocoding_allowed_ = false;
    bool amap_places_allowed_ = false;
    bool amap_reverse_geocoding_allowed_ = false;
    QString library_map_provider_ = QStringLiteral("none");
    QString map_style_ = QStringLiteral("roadmap");
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
    QObject* const google_map =
        dialog->findChild<QObject*>(QStringLiteral("googleLibraryMapSwitch"));
    QObject* const places =
        dialog->findChild<QObject*>(QStringLiteral("googlePlacesPermissionSwitch"));
    QObject* const reverse =
        dialog->findChild<QObject*>(QStringLiteral("googleReverseGeocodingPermissionSwitch"));
    QObject* const obsolete_osm_provider =
        dialog->findChild<QObject*>(QStringLiteral("openStreetMapProviderButton"));
    QObject* const map_style_controls =
        dialog->findChild<QObject*>(QStringLiteral("libraryMapStyleControls"));
    QObject* const satellite_style =
        dialog->findChild<QObject*>(QStringLiteral("googleSatelliteStyleButton"));
    QObject* const amap_web_field =
        dialog->findChild<QObject*>(QStringLiteral("amapWebServiceKeyField"));
    QObject* const amap_web_save =
        dialog->findChild<QObject*>(QStringLiteral("amapWebServiceKeySaveButton"));
    QObject* const amap_places =
        dialog->findChild<QObject*>(QStringLiteral("amapPlacesPermissionSwitch"));
    QObject* const amap_reverse =
        dialog->findChild<QObject*>(QStringLiteral("amapReverseGeocodingPermissionSwitch"));
    QObject* const amap_js_field = dialog->findChild<QObject*>(QStringLiteral("amapJsApiKeyField"));
    QObject* const amap_security_field =
        dialog->findChild<QObject*>(QStringLiteral("amapSecurityJsCodeField"));
    QObject* const amap_js_save =
        dialog->findChild<QObject*>(QStringLiteral("amapJsCredentialsSaveButton"));
    QObject* const amap_map = dialog->findChild<QObject*>(QStringLiteral("amapLibraryMapSwitch"));
    if (!require(
            field != nullptr && save != nullptr && remove != nullptr && google_map != nullptr
                && places != nullptr && reverse != nullptr && map_style_controls != nullptr
                && satellite_style != nullptr && amap_web_field != nullptr
                && amap_web_save != nullptr && amap_places != nullptr && amap_reverse != nullptr
                && amap_js_field != nullptr && amap_security_field != nullptr
                && amap_js_save != nullptr && amap_map != nullptr
                && obsolete_osm_provider == nullptr,
            "the packaged dialog exposes AMap and Google controls without an obsolete OSM selector"
        )
        || !require(
            !google_map->property("enabled").toBool() && !places->property("enabled").toBool()
                && !reverse->property("enabled").toBool(),
            "Google service permissions stay disabled before a key is stored"
        )
        || !require(
            !amap_places->property("enabled").toBool()
                && !amap_reverse->property("enabled").toBool(),
            "AMap service permissions stay disabled before a Web Service key is stored"
        )) {
        return EXIT_FAILURE;
    }

    const QString api_key = QStringLiteral("AIzaDialogContractKey_1234567890");
    field->setProperty("text", api_key);
    amap_web_field->setProperty("text", QStringLiteral("amap-unsaved-web-key"));
    amap_js_field->setProperty("text", QStringLiteral("amap-unsaved-js-key"));
    amap_security_field->setProperty("text", QStringLiteral("amap-unsaved-security-code"));
    QMetaObject::invokeMethod(dialog.get(), "closed");
    drainBindings();
    if (!require(
            field->property("text").toString().isEmpty()
                && amap_web_field->property("text").toString().isEmpty()
                && amap_js_field->property("text").toString().isEmpty()
                && amap_security_field->property("text").toString().isEmpty(),
            "closing the panel drops every unsaved provider credential"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            QMetaObject::invokeMethod(dialog.get(), "present"),
            "the settings panel can be reopened for interactive visibility checks"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();

    const QString amap_web_key = QStringLiteral("1234567890abcdef1234567890abcdef");
    amap_web_field->setProperty("text", amap_web_key);
    drainBindings();
    if (!require(
            amap_web_save->property("enabled").toBool() && click(amap_web_save)
                && preferences.amap_web_save_count == 1
                && preferences.saved_amap_web_value == amap_web_key
                && amap_web_field->property("text").toString().isEmpty()
                && amap_places->property("enabled").toBool()
                && amap_reverse->property("enabled").toBool(),
            "saving the AMap Web key clears the draft and unlocks only AMap Web services"
        )) {
        return EXIT_FAILURE;
    }

    const QString amap_js_key = QStringLiteral("abcdef1234567890abcdef1234567890");
    const QString amap_security = QStringLiteral("fedcba0987654321fedcba0987654321");
    amap_js_field->setProperty("text", amap_js_key);
    amap_security_field->setProperty("text", amap_security);
    drainBindings();
    if (!require(
            amap_js_save->property("enabled").toBool() && click(amap_js_save)
                && preferences.amap_js_save_count == 1
                && preferences.saved_amap_js_key == amap_js_key
                && preferences.saved_amap_security_code == amap_security
                && amap_js_field->property("text").toString().isEmpty()
                && amap_security_field->property("text").toString().isEmpty()
                && amap_map->property("enabled").toBool() && amap_map->property("checked").toBool()
                && preferences.libraryMapProvider() == QStringLiteral("amap")
                && dialog->property("libraryMapReady").toBool(),
            "AMap JS credentials atomically activate the first usable basemap"
        )
        || !require(
            click(amap_map) && preferences.libraryMapProvider() == QStringLiteral("none")
                && !amap_map->property("checked").toBool()
                && !dialog->property("libraryMapReady").toBool(),
            "clicking the AMap switch persists the provider identity instead of transient state"
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
            google_map->property("enabled").toBool() && places->property("enabled").toBool()
                && reverse->property("enabled").toBool() && google_map->property("checked").toBool()
                && preferences.libraryMapProvider() == QStringLiteral("google")
                && dialog->property("libraryMapReady").toBool(),
            "storing the first available Google key activates its WebView provider"
        )
        || !require(
            click(google_map) && preferences.libraryMapProvider() == QStringLiteral("none")
                && click(google_map) && preferences.libraryMapProvider() == QStringLiteral("google")
                && google_map->property("checked").toBool()
                && dialog->property("libraryMapReady").toBool(),
            "the Google switch reliably disables and restores the shared WebView map"
        )
        || !require(
            click(satellite_style) && preferences.mapStyle() == QStringLiteral("satellite"),
            "the shared map style is delegated to provider preferences"
        )
        || !require(
            click(remove) && preferences.remove_count == 1
                && !google_map->property("enabled").toBool(),
            "removing the stored key returns the panel to its safe default"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "map_provider_settings_dialog_test.moc"
