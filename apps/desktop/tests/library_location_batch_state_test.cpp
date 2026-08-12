#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FakeCoordinateBatchController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap libraryCoordinateBatchPreview READ preview NOTIFY libraryMetadataChanged)
    Q_PROPERTY(QVariantMap libraryMetadataBatchReceipt READ receipt NOTIFY libraryMetadataChanged)
    Q_PROPERTY(bool libraryMetadataBusy READ busy NOTIFY libraryMetadataChanged)
    Q_PROPERTY(QString libraryMetadataStatusCode READ statusCode NOTIFY libraryMetadataChanged)
    Q_PROPERTY(QString libraryMetadataErrorText READ errorText NOTIFY libraryMetadataChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] QVariantMap preview() const { return preview_; }
    [[nodiscard]] QVariantMap receipt() const { return receipt_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] QString statusCode() const { return status_code_; }
    [[nodiscard]] QString errorText() const { return error_text_; }

    Q_INVOKABLE void previewLibraryCoordinateBatch(
        const QVariantList& targets,
        const QString& mode,
        const double latitude,
        const double longitude,
        const QString& place_name,
        const QString& source_label
    ) {
        ++preview_count;
        requested_targets = targets;
        requested_mode = mode;
        requested_latitude = latitude;
        requested_longitude = longitude;
        requested_place_name = place_name;
        requested_source_label = source_label;
        busy_ = true;
        status_code_ = QStringLiteral("previewing");
        emit libraryMetadataChanged();
    }

    Q_INVOKABLE void clearLibraryMetadata() {
        if (busy_)
            return;
        preview_.clear();
        receipt_.clear();
        status_code_ = QStringLiteral("idle");
        error_text_.clear();
        emit libraryMetadataChanged();
    }

    Q_INVOKABLE void applyLibraryCoordinateBatch(const QString& preview_id) {
        ++apply_count;
        requested_preview_id = preview_id;
        busy_ = true;
        status_code_ = QStringLiteral("applying");
        emit libraryMetadataChanged();
    }

    void finishPreview() {
        busy_ = false;
        status_code_ = QStringLiteral("ready");
        preview_ = {
            {QStringLiteral("previewId"), QStringLiteral("preview-1")},
            {QStringLiteral("mode"), requested_mode},
            {QStringLiteral("latitude"), requested_latitude},
            {QStringLiteral("longitude"), requested_longitude},
            {QStringLiteral("placeName"), requested_place_name},
            {QStringLiteral("applicablePhotoCount"), 2},
            {QStringLiteral("replacementPhotoCount"), 0},
        };
        emit libraryMetadataChanged();
    }

    void finishApply() {
        busy_ = false;
        status_code_ = QStringLiteral("applied");
        preview_.clear();
        receipt_ = {{QStringLiteral("appliedPhotoCount"), 2}};
        emit libraryMetadataChanged();
    }

    int preview_count = 0;
    int apply_count = 0;
    QVariantList requested_targets;
    QString requested_mode;
    double requested_latitude = 0;
    double requested_longitude = 0;
    QString requested_place_name;
    QString requested_source_label;
    QString requested_preview_id;

  signals:
    void libraryMetadataChanged();

  private:
    QVariantMap preview_;
    QVariantMap receipt_;
    QString status_code_ = QStringLiteral("idle");
    QString error_text_;
    bool busy_ = false;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition)
        std::cerr << "Library location batch state contract failed: " << message << '\n';
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
    const QVariantList& arguments = {}
) {
    QVariant returned;
    bool invoked = false;
    switch (arguments.size()) {
    case 0:
        invoked = QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, returned));
        break;
    case 4:
        invoked = QMetaObject::invokeMethod(
            object,
            method,
            Q_RETURN_ARG(QVariant, returned),
            Q_ARG(QVariant, arguments.at(0)),
            Q_ARG(QVariant, arguments.at(1)),
            Q_ARG(QVariant, arguments.at(2)),
            Q_ARG(QVariant, arguments.at(3))
        );
        break;
    case 5:
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
        break;
    default:
        return false;
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
        QStringLiteral("Shadow.LibraryLocationBatchContract"),
        QStringLiteral("LibraryLocationBatchState")
    );

    FakeCoordinateBatchController controller;
    std::unique_ptr<QObject> state(component.createWithInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&controller)}}
    ));
    if (!require(state != nullptr, "state component should load from its packaged module")) {
        for (const QQmlError& error : component.errors())
            std::cerr << error.toString().toStdString() << '\n';
        return EXIT_FAILURE;
    }

    const QVariantList targets{
        QVariantMap{{QStringLiteral("photoId"), QStringLiteral("photo-a")}},
        QVariantMap{{QStringLiteral("photoId"), QStringLiteral("photo-b")}},
    };
    if (!require(
            invokeBool(
                state.get(),
                "present",
                {targets, false, 0.0, 0.0, QString()}
            ),
            "selection should open the lifecycle"
        )
        || !require(
            !invokeBool(
                state.get(),
                "proposeCoordinate",
                {91.0, 10.0, QString(), QStringLiteral("manual-map")}
            ),
            "invalid coordinates should be rejected"
        )
        || !require(
            invokeBool(
                state.get(),
                "proposeCoordinate",
                {31.2304, 121.4737, QStringLiteral("Shanghai"),
                 QStringLiteral("place-search:Shanghai")}
            ),
            "valid coordinates should be accepted"
        )
        || !require(
            invokeBool(state.get(), "previewAssignment"),
            "preview should be admitted"
        )) {
        return EXIT_FAILURE;
    }
    if (!require(controller.preview_count == 1, "one preview request should be sent")
        || !require(controller.requested_mode == QStringLiteral("missing"),
                    "missing-only must be the default policy")
        || !require(closeTo(controller.requested_latitude, 31.2304),
                    "preview should preserve latitude")
        || !require(controller.requested_source_label
                        == QStringLiteral("place-search:Shanghai"),
                    "preview should preserve provenance")) {
        return EXIT_FAILURE;
    }

    controller.finishPreview();
    drainBindings();
    if (!require(state->property("previewReady").toBool(),
                 "finished preview should become ready")
        || !require(invokeBool(state.get(), "applyAssignment"),
                    "ready preview should apply")
        || !require(controller.requested_preview_id == QStringLiteral("preview-1"),
                    "apply should use the opaque preview id")) {
        return EXIT_FAILURE;
    }
    controller.finishApply();
    drainBindings();
    if (!require(state->property("applied").toBool(),
                 "terminal receipt should mark the lifecycle applied")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#include "library_location_batch_state_test.moc"
