#include <QGuiApplication>
#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeLocationCompletionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList locationCompletionGroups READ groups CONSTANT)
    Q_PROPERTY(bool locationCompletionBusy READ busy CONSTANT)
    Q_PROPERTY(bool locationCompletionTruncated READ truncated CONSTANT)
    Q_PROPERTY(QString locationCompletionErrorText READ errorText CONSTANT)
    Q_PROPERTY(QVariantList locationReferenceLibraries READ locationReferenceLibraries CONSTANT)
    Q_PROPERTY(bool locationReferenceBusy READ locationReferenceBusy CONSTANT)
    Q_PROPERTY(QString locationReferenceErrorText READ locationReferenceErrorText CONSTANT)

  public:
    [[nodiscard]] QVariantList groups() const {
        return {QVariantMap{
            {QStringLiteral("id"), QStringLiteral("event-1")},
            {QStringLiteral("startedAt"), 1'723'456'000LL},
            {QStringLiteral("endedAt"), 1'723'456'600LL},
            {QStringLiteral("targetCount"), 2},
            {QStringLiteral("anchorCount"), 1},
            {QStringLiteral("hasSuggestion"), true},
            {QStringLiteral("latitude"), 31.2304},
            {QStringLiteral("longitude"), 121.4737},
            {QStringLiteral("placeName"), QStringLiteral("Shanghai")},
            {QStringLiteral("targets"), QVariantList{QVariantMap{
                {QStringLiteral("photoId"), QStringLiteral("photo-a")},
                {QStringLiteral("representationId"), QStringLiteral("raw-a")},
                {QStringLiteral("visualHandle"), QStringLiteral("visual-a")},
                {QStringLiteral("sourcePath"), QStringLiteral("/photos/photo-a.raw")},
                {QStringLiteral("title"), QStringLiteral("Photo A")},
                {QStringLiteral("sourceAvailable"), true},
                {QStringLiteral("visualWidth"), 2048},
                {QStringLiteral("visualHeight"), 1365},
            }}},
        }};
    }
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool truncated() const noexcept { return false; }
    [[nodiscard]] QString errorText() const { return {}; }
    [[nodiscard]] QVariantList locationReferenceLibraries() const { return {}; }
    [[nodiscard]] bool locationReferenceBusy() const noexcept { return false; }
    [[nodiscard]] QString locationReferenceErrorText() const { return {}; }

    Q_INVOKABLE void requestLocationCompletion(const qlonglong start, const qlonglong end) {
        requested_start = start;
        requested_end = end;
        ++request_count;
    }
    Q_INVOKABLE void refreshLocationReferenceLibraries() { ++reference_refresh_count; }
    Q_INVOKABLE void addLocationReferenceLibrary(const QUrl&, const qlonglong) {}
    Q_INVOKABLE void removeLocationReferenceLibrary(const QString&) {}
    Q_INVOKABLE QString locationCompletionVisualSource(const QString&) const {
        return {};
    }

    qlonglong requested_start = -1;
    qlonglong requested_end = -1;
    int request_count = 0;
    int reference_refresh_count = 0;
};

class FakeLocationCompletionWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* controller READ controller CONSTANT)

  public:
    explicit FakeLocationCompletionWorkspace(FakeLocationCompletionController* controller) :
        controller_(controller) {}

    [[nodiscard]] QObject* controller() const { return controller_; }
    Q_INVOKABLE bool openLocationBatchForTargets(
        const QVariantList& targets,
        const bool has_coordinate,
        const double latitude,
        const double longitude,
        const QString& place_name,
        const QString& source_label
    ) {
        opened_targets = targets;
        opened_has_coordinate = has_coordinate;
        opened_latitude = latitude;
        opened_longitude = longitude;
        opened_place_name = place_name;
        opened_source_label = source_label;
        return !targets.isEmpty();
    }
    Q_INVOKABLE void selectPhoto(const QVariantMap&, const int) {}

    QObject* controller_ = nullptr;
    QVariantList opened_targets;
    bool opened_has_coordinate = false;
    double opened_latitude = 0.0;
    double opened_longitude = 0.0;
    QString opened_place_name;
    QString opened_source_label;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Library location completion Gallery contract failed: " << message << '\n';
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
        QStringLiteral("Shadow.LibraryLocationCompletionGalleryContract"),
        QStringLiteral("LibraryLocationCompletionGallery")
    );

    FakeLocationCompletionController controller;
    FakeLocationCompletionWorkspace workspace(&controller);
    std::unique_ptr<QObject> gallery{component.createWithInitialProperties({
        {QStringLiteral("workspace"), QVariant::fromValue(&workspace)},
    })};
    if (!require(gallery != nullptr, "Gallery should load from its packaged module")) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    bool valid = true;
    valid &= require(
        QMetaObject::invokeMethod(gallery.get(), "present"),
        "present should request the initial unbounded event query"
    );
    drainBindings();
    valid &= require(
        controller.request_count == 1 && controller.requested_start == 0
            && controller.requested_end == 0 && controller.reference_refresh_count == 1,
        "opening refreshes events and reference evidence"
    );
    valid &= require(
        gallery->findChild<QObject*>(QStringLiteral("locationCompletionStartField")) != nullptr
            && gallery->findChild<QObject*>(QStringLiteral("locationCompletionEndField")) != nullptr
            && gallery->findChild<QObject*>(QStringLiteral("locationCompletionRefreshButton")) != nullptr
            && gallery->findChild<QObject*>(QStringLiteral("locationCompletionEventGallery")) != nullptr,
        "range controls and the grouped Gallery surface should be reachable"
    );
    QVariant opened;
    valid &= require(
        QMetaObject::invokeMethod(
            gallery.get(),
            "openEvent",
            Q_RETURN_ARG(QVariant, opened),
            Q_ARG(QVariant, controller.groups().constFirst())
        ),
        "an event Gallery card should route through the explicit batch preview boundary"
    );
    valid &= require(
        opened.toBool() && workspace.opened_targets.size() == 1
            && workspace.opened_targets.constFirst().toMap().value(
                QStringLiteral("photoId")
            ) == QStringLiteral("photo-a")
            && workspace.opened_targets.constFirst().toMap().value(
                QStringLiteral("representationId")
            ) == QStringLiteral("raw-a")
            && workspace.opened_has_coordinate
            && workspace.opened_place_name == QStringLiteral("Shanghai"),
        "the Gallery preserves representation identity and leaves map confirmation in the existing batch flow"
    );
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "library_location_completion_gallery_test.moc"
