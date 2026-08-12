#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FakeCoordinateBatchController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap libraryCoordinateBatchPreview READ preview CONSTANT)
    Q_PROPERTY(QVariantMap libraryMetadataBatchReceipt READ receipt CONSTANT)
    Q_PROPERTY(bool libraryMetadataBusy READ busy NOTIFY libraryMetadataChanged)
    Q_PROPERTY(QString libraryMetadataStatusCode READ statusCode CONSTANT)
    Q_PROPERTY(QString libraryMetadataErrorText READ errorText CONSTANT)

  public:
    [[nodiscard]] QVariantMap preview() const {
        return {};
    }
    [[nodiscard]] QVariantMap receipt() const {
        return {};
    }
    [[nodiscard]] bool busy() const noexcept {
        return busy_;
    }
    [[nodiscard]] QString statusCode() const {
        return QStringLiteral("idle");
    }
    [[nodiscard]] QString errorText() const {
        return {};
    }

    Q_INVOKABLE void clearLibraryMetadata() {}
    Q_INVOKABLE void previewLibraryCoordinateBatch(
        const QVariantList&,
        const QString&,
        double,
        double,
        const QString&,
        const QString&
    ) {}
    Q_INVOKABLE void applyLibraryCoordinateBatch(const QString&) {}

  signals:
    void libraryMetadataChanged();

  private:
    bool busy_ = false;
};

class FakeMapController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY stateChanged)
    Q_PROPERTY(bool providerSelected READ providerSelected CONSTANT)
    Q_PROPERTY(bool providerAvailable READ providerAvailable CONSTANT)
    Q_PROPERTY(bool providerRegionAvailable READ providerRegionAvailable CONSTANT)
    Q_PROPERTY(QString providerName READ providerName CONSTANT)
    Q_PROPERTY(QString providerId READ providerId CONSTANT)
    Q_PROPERTY(double centerLatitude READ centerLatitude NOTIFY centerChanged)
    Q_PROPERTY(double centerLongitude READ centerLongitude NOTIFY centerChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel NOTIFY centerChanged)

  public:
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool providerSelected() const noexcept {
        return true;
    }
    [[nodiscard]] bool providerAvailable() const noexcept {
        return true;
    }
    [[nodiscard]] bool providerRegionAvailable() const noexcept {
        return true;
    }
    [[nodiscard]] QString providerName() const {
        return QStringLiteral("AMap");
    }
    [[nodiscard]] QString providerId() const {
        return QStringLiteral("amap");
    }
    [[nodiscard]] double centerLatitude() const noexcept {
        return latitude_;
    }
    [[nodiscard]] double centerLongitude() const noexcept {
        return longitude_;
    }
    [[nodiscard]] double zoomLevel() const noexcept {
        return zoom_;
    }

    void setActive(const bool active) {
        if (active_ == active)
            return;
        active_ = active;
        emit stateChanged();
    }

    Q_INVOKABLE void setLanguage(const QString&) {}
    Q_INVOKABLE void beginMapContext(const double latitude, const double longitude) {
        latitude_ = latitude;
        longitude_ = longitude;
        emit centerChanged();
    }
    Q_INVOKABLE void
    navigateToContext(const double latitude, const double longitude, const double zoom) {
        latitude_ = latitude;
        longitude_ = longitude;
        zoom_ = zoom;
        emit centerChanged();
    }
    Q_INVOKABLE void setPlacementActive(const bool active) {
        placement_active = active;
    }
    Q_INVOKABLE void
    setPendingCoordinate(const bool present, const double latitude, const double longitude) {
        pending_present = present;
        pending_latitude = latitude;
        pending_longitude = longitude;
    }

    void proposeCoordinate(const double latitude, const double longitude) {
        emit coordinateProposed(latitude, longitude);
    }

    bool placement_active = false;
    bool pending_present = false;
    double pending_latitude = 0.0;
    double pending_longitude = 0.0;

  signals:
    void stateChanged();
    void centerChanged();
    void coordinateProposed(double latitude, double longitude);

  private:
    bool active_ = false;
    double latitude_ = 20.0;
    double longitude_ = 0.0;
    double zoom_ = 2.5;
};

class FakePlaceSearchService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(QVariantList results READ results CONSTANT)
    Q_PROPERTY(QString errorText READ errorText CONSTANT)

  public:
    [[nodiscard]] bool available() const noexcept {
        return true;
    }
    [[nodiscard]] bool busy() const noexcept {
        return false;
    }
    [[nodiscard]] QVariantList results() const {
        return {};
    }
    [[nodiscard]] QString errorText() const {
        return {};
    }

    Q_INVOKABLE void search(const QString&, double, double) {}
    Q_INVOKABLE void clear() {
        ++clear_count;
    }

    int clear_count = 0;

  signals:
    void stateChanged();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition)
        std::cerr << "Library location batch dialog contract failed: " << message << '\n';
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool
invokeBool(QObject* const object, const char* const method, const QVariantList& arguments = {}) {
    QVariant returned;
    bool invoked = false;
    if (arguments.empty()) {
        invoked = QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, returned));
    } else if (arguments.size() == 5) {
        invoked = QMetaObject::invokeMethod(
            object,
            method,
            Q_RETURN_ARG(QVariant, returned),
            Q_ARG(QVariant, arguments.at(0)),
            Q_ARG(QVariant, arguments.at(1)),
            Q_ARG(QVariant, arguments.at(2)),
            Q_ARG(QVariant, arguments.at(3)),
            Q_ARG(QVariant, arguments.at(4))
        );
    }
    drainBindings();
    return invoked && returned.toBool();
}

[[nodiscard]] bool closeTo(const double actual, const double expected) {
    return std::abs(actual - expected) < 0.000'001;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.LibraryLocationBatchDialogContract"),
        QStringLiteral("LibraryLocationBatchDialog")
    );

    FakeCoordinateBatchController controller;
    FakeMapController map_controller;
    FakePlaceSearchService place_search;
    std::unique_ptr<QObject> dialog{component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("mapController"), QVariant::fromValue(&map_controller)},
        {QStringLiteral("placeSearchService"), QVariant::fromValue(&place_search)},
        {QStringLiteral("nativeWebMapAllowed"), false},
    })};
    if (!require(dialog != nullptr, "dialog should load from its packaged module")) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto* const dialog_window = qobject_cast<QQuickWindow*>(dialog.get());
    if (!require(dialog_window != nullptr, "the dialog should be a real window"))
        return EXIT_FAILURE;

    const QVariantList targets{
        QVariantMap{{QStringLiteral("photoId"), QStringLiteral("photo-a")}},
    };
    bool valid = require(
        invokeBool(dialog.get(), "present", {targets, false, 0.0, 0.0, QString{}}),
        "a selected photo should present the dialog"
    );
    valid &=
        require(dialog->property("visible").toBool(), "the presented dialog should be visible");
    valid &= require(map_controller.active(), "the presented dialog should activate map placement");

    QObject* const search_field =
        dialog->findChild<QObject*>(QStringLiteral("libraryMapAmapSearchField"));
    valid &= require(search_field != nullptr, "the dialog should expose its place-search field");
    QObject* const place_search_item =
        dialog->findChild<QObject*>(QStringLiteral("libraryMapPlaceSearch"));
    valid &= require(
        place_search_item != nullptr,
        "the dialog should compose the place-search owner"
    );
    if (place_search_item != nullptr)
        valid &= require(
            place_search_item->property("implicitHeight").toDouble() > 0.0,
            "the dialog search should reserve its dynamic result height in the map column"
        );

    auto* const close_button =
        dialog->findChild<QQuickItem*>(QStringLiteral("locationBatchCloseButton"));
    valid &= require(close_button != nullptr, "the dialog should expose an explicit close action");
    if (close_button != nullptr) {
        const QPointF close_position = close_button->mapToScene(
            QPointF{close_button->width() / 2.0, close_button->height() / 2.0}
        );
        QTest::mouseClick(dialog_window, Qt::LeftButton, Qt::NoModifier, close_position.toPoint());
        drainBindings();
    }
    valid &=
        require(!dialog->property("visible").toBool(), "the close action should hide the dialog");
    valid &= require(
        !map_controller.active() && !map_controller.placement_active,
        "closing should release the shared map controller"
    );

    valid &= require(
        invokeBool(dialog.get(), "present", {targets, false, 0.0, 0.0, QString{}}),
        "the dialog should reopen after explicit dismissal"
    );
    map_controller.proposeCoordinate(31.2304, 121.4737);
    drainBindings();

    QObject* const latitude_field =
        dialog->findChild<QObject*>(QStringLiteral("locationBatchLatitudeField"));
    QObject* const longitude_field =
        dialog->findChild<QObject*>(QStringLiteral("locationBatchLongitudeField"));
    valid &= require(
        latitude_field != nullptr && longitude_field != nullptr,
        "coordinate fields should be reachable"
    );
    valid &= require(
        map_controller.pending_present && closeTo(map_controller.pending_latitude, 31.2304)
            && closeTo(map_controller.pending_longitude, 121.4737),
        "a map click should become the pending batch coordinate"
    );
    if (latitude_field != nullptr && longitude_field != nullptr) {
        valid &= require(
            latitude_field->property("text").toString().startsWith(QStringLiteral("31.2304"))
                && longitude_field->property("text").toString().startsWith(
                    QStringLiteral("121.4737")
                ),
            "a map click should update the visible coordinate fields"
        );
    }

    valid &= require(
        invokeBool(dialog.get(), "requestDismiss"),
        "the Escape dismissal path should be admitted while idle"
    );
    valid &= require(!dialog->property("visible").toBool(), "dismissal should close the dialog");
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "library_location_batch_dialog_test.moc"
