#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QVariant>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FakeLibraryMetadataController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool libraryMetadataBusy READ busy NOTIFY libraryMetadataChanged)
    Q_PROPERTY(QString libraryMetadataStatusCode READ statusCode NOTIFY libraryMetadataChanged)
    Q_PROPERTY(QString libraryMetadataErrorText READ errorText NOTIFY libraryMetadataChanged)

  public:
    [[nodiscard]] bool busy() const {
        return busy_;
    }

    [[nodiscard]] QString statusCode() const {
        return status_code_;
    }

    [[nodiscard]] QString errorText() const {
        return error_text_;
    }

    Q_INVOKABLE void setLibraryCoordinates(
        const QString& photo_id,
        const QString& mode,
        const double latitude,
        const double longitude,
        const QString& place_name
    ) {
        ++request_count;
        requested_photo_id = photo_id;
        requested_mode = mode;
        requested_latitude = latitude;
        requested_longitude = longitude;
        requested_place_name = place_name;
        busy_ = true;
        status_code_ = QStringLiteral("saving");
        error_text_.clear();
        emit libraryMetadataChanged();
    }

    void finishFailure(const QString& error) {
        busy_ = false;
        status_code_ = QStringLiteral("failed");
        error_text_ = error;
        emit libraryMetadataChanged();
    }

    void finishSuccess() {
        busy_ = false;
        status_code_ = QStringLiteral("saved");
        error_text_.clear();
        emit libraryMetadataChanged();
    }

    void setBusy(const bool busy) {
        busy_ = busy;
        emit libraryMetadataChanged();
    }

    int request_count = 0;
    QString requested_photo_id;
    QString requested_mode;
    double requested_latitude = 0.0;
    double requested_longitude = 0.0;
    QString requested_place_name;

  signals:
    void libraryMetadataChanged();

  private:
    bool busy_ = false;
    QString status_code_;
    QString error_text_;
};

class FakeLibraryMapWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedPhotoId READ selectedPhotoId NOTIFY selectedPhotoIdChanged)
    Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedHasCoordinates READ selectedHasCoordinates NOTIFY selectionChanged)
    Q_PROPERTY(double selectedLatitude READ selectedLatitude NOTIFY selectionChanged)
    Q_PROPERTY(double selectedLongitude READ selectedLongitude NOTIFY selectionChanged)
    Q_PROPERTY(QObject* controller READ controller CONSTANT)

  public:
    explicit FakeLibraryMapWorkspace(
        FakeLibraryMetadataController* controller,
        QObject* parent = nullptr
    ) : QObject(parent), controller_(controller) {}

    [[nodiscard]] QString selectedPhotoId() const {
        return photo_id_;
    }

    [[nodiscard]] QString selectedTitle() const {
        return title_;
    }

    [[nodiscard]] bool selectedHasCoordinates() const {
        return has_coordinates_;
    }

    [[nodiscard]] double selectedLatitude() const {
        return latitude_;
    }

    [[nodiscard]] double selectedLongitude() const {
        return longitude_;
    }

    [[nodiscard]] QObject* controller() const {
        return controller_;
    }

    void select(
        const QString& photo_id,
        const QString& title,
        const bool has_coordinates,
        const double latitude,
        const double longitude
    ) {
        const bool identity_changed = photo_id_ != photo_id;
        photo_id_ = photo_id;
        title_ = title;
        has_coordinates_ = has_coordinates;
        latitude_ = latitude;
        longitude_ = longitude;
        emit selectionChanged();
        if (identity_changed) {
            emit selectedPhotoIdChanged();
        }
    }

  signals:
    void selectedPhotoIdChanged();
    void selectionChanged();

  private:
    FakeLibraryMetadataController* controller_;
    QString photo_id_;
    QString title_;
    bool has_coordinates_ = false;
    double latitude_ = 0.0;
    double longitude_ = 0.0;
};

class SavedLocationRecorder final : public QObject {
    Q_OBJECT

  public slots:
    void onLocationSaved(const QString& photo_id, const double latitude, const double longitude) {
        ++count;
        saved_photo_id = photo_id;
        saved_latitude = latitude;
        saved_longitude = longitude;
    }

  public:
    int count = 0;
    QString saved_photo_id;
    double saved_latitude = 0.0;
    double saved_longitude = 0.0;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Library map location placement contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool invokeBool(
    QObject* object,
    const char* method,
    const QVariant& first = {},
    const QVariant& second = {}
) {
    QVariant returned;
    bool invoked = false;
    if (first.isValid() && second.isValid()) {
        invoked = QMetaObject::invokeMethod(
            object,
            method,
            Q_RETURN_ARG(QVariant, returned),
            Q_ARG(QVariant, first),
            Q_ARG(QVariant, second)
        );
    } else if (first.isValid()) {
        invoked = QMetaObject::invokeMethod(
            object,
            method,
            Q_RETURN_ARG(QVariant, returned),
            Q_ARG(QVariant, first)
        );
    } else {
        invoked = QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, returned));
    }
    drainBindings();
    return invoked && returned.toBool();
}

[[nodiscard]] bool closeTo(const double actual, const double expected) {
    return std::abs(actual - expected) < 0.000'001;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.LibraryMapLocationPlacementContract"),
        QStringLiteral("LibraryMapLocationPlacementState")
    );

    FakeLibraryMetadataController controller;
    FakeLibraryMapWorkspace workspace{&controller};
    workspace.select(
        QStringLiteral("photo-a"),
        QStringLiteral("Mountain dawn"),
        true,
        31.2304,
        121.4737
    );
    std::unique_ptr<QObject> placement{component.createWithInitialProperties({
        {QStringLiteral("workspace"), QVariant::fromValue(&workspace)},
    })};
    if (!placement) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    SavedLocationRecorder recorder;
    QObject::connect(
        placement.get(),
        SIGNAL(locationSaved(QString, double, double)),
        &recorder,
        SLOT(onLocationSaved(QString, double, double))
    );

    if (!require(
            invokeBool(placement.get(), "begin"),
            "a selected idle photo enters placement mode"
        )
        || !require(
            placement->property("active").toBool()
                && placement->property("hasPendingCoordinate").toBool()
                && placement->property("photoId").toString() == QStringLiteral("photo-a")
                && placement->property("photoTitle").toString() == QStringLiteral("Mountain dawn")
                && closeTo(placement->property("pendingLatitude").toDouble(), 31.2304)
                && closeTo(placement->property("pendingLongitude").toDouble(), 121.4737),
            "placement snapshots identity and the effective existing coordinate"
        )
        || !require(
            !invokeBool(placement.get(), "proposeCoordinate", QVariant{91.0}, QVariant{0.0}),
            "out-of-range coordinates are rejected"
        )
        || !require(
            invokeBool(placement.get(), "proposeCoordinate", QVariant{35.6762}, QVariant{139.6503}),
            "a bounded map coordinate replaces the candidate"
        )
        || !require(
            invokeBool(placement.get(), "commit") && controller.request_count == 1
                && controller.requested_photo_id == QStringLiteral("photo-a")
                && controller.requested_mode == QStringLiteral("set")
                && closeTo(controller.requested_latitude, 35.6762)
                && closeTo(controller.requested_longitude, 139.6503)
                && controller.requested_place_name.isEmpty()
                && placement->property("saving").toBool(),
            "confirmation delegates one exact coordinate mutation without a stale place name"
        )) {
        return EXIT_FAILURE;
    }

    controller.finishFailure(QStringLiteral("catalog busy"));
    drainBindings();
    if (!require(
            placement->property("active").toBool()
                && placement->property("hasPendingCoordinate").toBool()
                && !placement->property("saving").toBool() && placement->property("failed").toBool()
                && placement->property("errorText").toString() == QStringLiteral("catalog busy"),
            "failure preserves the candidate and exposes a retryable diagnostic"
        )
        || !require(
            invokeBool(placement.get(), "commit") && controller.request_count == 2,
            "the preserved candidate can be retried explicitly"
        )) {
        return EXIT_FAILURE;
    }

    controller.finishSuccess();
    drainBindings();
    if (!require(
            !placement->property("active").toBool() && !placement->property("saving").toBool()
                && !placement->property("hasPendingCoordinate").toBool() && recorder.count == 1
                && recorder.saved_photo_id == QStringLiteral("photo-a")
                && closeTo(recorder.saved_latitude, 35.6762)
                && closeTo(recorder.saved_longitude, 139.6503),
            "success emits the persisted identity once and retires placement"
        )) {
        return EXIT_FAILURE;
    }

    workspace.select(QStringLiteral("photo-a"), QStringLiteral("Mountain dawn"), false, 0.0, 0.0);
    if (!require(
            invokeBool(placement.get(), "begin")
                && !placement->property("hasPendingCoordinate").toBool(),
            "a photo without effective GPS begins without a fabricated pin"
        )) {
        return EXIT_FAILURE;
    }
    workspace.select(QStringLiteral("photo-b"), QStringLiteral("Harbor dusk"), false, 0.0, 0.0);
    drainBindings();
    if (!require(
            !placement->property("active").toBool()
                && placement->property("photoId").toString().isEmpty(),
            "changing selection cancels an uncommitted candidate"
        )) {
        return EXIT_FAILURE;
    }

    controller.setBusy(true);
    return require(
               !invokeBool(placement.get(), "begin"),
               "another metadata transaction prevents placement admission"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "library_map_location_placement_test.moc"
