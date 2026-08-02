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

class FakeLibraryMapController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(qulonglong libraryMapPhotoCount READ libraryMapPhotoCount CONSTANT)
    Q_PROPERTY(bool libraryMapBusy READ libraryMapBusy CONSTANT)

  public:
    [[nodiscard]] qulonglong libraryMapPhotoCount() const noexcept {
        return 5;
    }

    [[nodiscard]] bool libraryMapBusy() const noexcept {
        return false;
    }
};

class FakeGoogleMapTilesService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(QString copyrightText READ copyrightText CONSTANT)

  public:
    [[nodiscard]] bool busy() const noexcept {
        return false;
    }

    [[nodiscard]] QString copyrightText() const {
        return QStringLiteral("Google test attribution");
    }
};

class FakeLibraryMapWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* controller READ controller CONSTANT)
    Q_PROPERTY(QObject* googleMapTilesService READ googleMapTilesService CONSTANT)

  public:
    FakeLibraryMapWorkspace(
        FakeLibraryMapController* const controller,
        FakeGoogleMapTilesService* const google_map_tiles_service
    ) : controller_(controller), google_map_tiles_service_(google_map_tiles_service) {}

    [[nodiscard]] QObject* controller() const noexcept {
        return controller_;
    }

    [[nodiscard]] QObject* googleMapTilesService() const noexcept {
        return google_map_tiles_service_;
    }

  private:
    FakeLibraryMapController* controller_ = nullptr;
    FakeGoogleMapTilesService* google_map_tiles_service_ = nullptr;
};

class ConfigureRequestObserver final : public QObject {
    Q_OBJECT

  public:
    int request_count = 0;

  public slots:
    void observeRequest() {
        ++request_count;
    }
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Library-map provider overlay contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.LibraryMapProviderOverlayContract"),
        QStringLiteral("LibraryMapProviderOverlay")
    );

    FakeLibraryMapController controller;
    FakeGoogleMapTilesService google_map_tiles_service;
    FakeLibraryMapWorkspace workspace{&controller, &google_map_tiles_service};
    std::unique_ptr<QObject> overlay{component.createWithInitialProperties({
        {QStringLiteral("workspace"), QVariant::fromValue(&workspace)},
        {QStringLiteral("googleProviderSelected"), false},
        {QStringLiteral("googleProviderAvailable"), false},
        {QStringLiteral("googleStatusMessage"), QString{}},
        {QStringLiteral("width"), 900.0},
        {QStringLiteral("height"), 600.0},
    })};
    if (!overlay) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QObject* const setup_panel =
        overlay->findChild<QObject*>(QStringLiteral("libraryMapSetupPanel"));
    QObject* const configure_button =
        overlay->findChild<QObject*>(QStringLiteral("libraryMapConfigureButton"));
    ConfigureRequestObserver observer;
    const bool connected = QObject::connect(
        overlay.get(),
        SIGNAL(configureRequested()),
        &observer,
        SLOT(observeRequest())
    );
    if (!require(
            setup_panel != nullptr && configure_button != nullptr && connected,
            "the packaged overlay exposes one map-setup action"
        )
        || !require(
            setup_panel->property("visible").toBool(),
            "an unavailable basemap presents setup guidance instead of a blank provider"
        )
        || !require(
            QMetaObject::invokeMethod(configure_button, "clicked"),
            "the setup action accepts a user click"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            observer.request_count == 1,
            "the setup action emits one request for the application-owned settings dialog"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("googleProviderAvailable", true);
    overlay->setProperty("googleProviderSelected", true);
    drainBindings();
    return require(
               !setup_panel->property("visible").toBool(),
               "the setup guidance leaves the map after Google tiles become available"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "library_map_provider_overlay_test.moc"
