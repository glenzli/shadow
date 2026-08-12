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

class FakeLibraryMapWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* controller READ controller CONSTANT)

  public:
    explicit FakeLibraryMapWorkspace(FakeLibraryMapController* const controller) :
        controller_(controller) {}

    [[nodiscard]] QObject* controller() const noexcept {
        return controller_;
    }

  private:
    FakeLibraryMapController* controller_ = nullptr;
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
    FakeLibraryMapWorkspace workspace{&controller};
    std::unique_ptr<QObject> overlay{component.createWithInitialProperties({
        {QStringLiteral("workspace"), QVariant::fromValue(&workspace)},
        {QStringLiteral("providerSelected"), false},
        {QStringLiteral("providerAvailable"), false},
        {QStringLiteral("providerRegionAvailable"), true},
        {QStringLiteral("providerName"), QString{}},
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

    overlay->setProperty("providerAvailable", true);
    overlay->setProperty("providerSelected", true);
    overlay->setProperty("providerName", QStringLiteral("Google Maps"));
    drainBindings();
    bool valid = require(
        !overlay->property("visible").toBool(),
        "the setup guidance leaves the map after the WebView provider becomes available"
    );
    overlay->setProperty("providerRegionAvailable", false);
    drainBindings();
    valid &= require(
        overlay->property("providerRegionAvailable").toBool() == false,
        "an unsupported manual-provider region shows guidance instead of a blank map"
    );
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "library_map_provider_overlay_test.moc"
