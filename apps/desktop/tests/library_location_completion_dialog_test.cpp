#include <QDate>
#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QString>
#include <QTime>
#include <QTimeZone>
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
        return {
            QVariantMap{
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
                    {QStringLiteral("title"), QStringLiteral("Photo A")},
                }}},
            },
        };
    }
    [[nodiscard]] bool busy() const noexcept {
        return false;
    }
    [[nodiscard]] bool truncated() const noexcept {
        return false;
    }
    [[nodiscard]] QString errorText() const {
        return {};
    }

    Q_INVOKABLE void requestLocationCompletion(
        const qlonglong start_unix_seconds,
        const qlonglong end_unix_seconds
    ) {
        start = start_unix_seconds;
        end = end_unix_seconds;
        ++request_count;
    }

    [[nodiscard]] QVariantList locationReferenceLibraries() const {
        return {QVariantMap{
            {QStringLiteral("id"), QStringLiteral("phone")},
            {QStringLiteral("rootPath"), QStringLiteral("/photos/phone")},
            {QStringLiteral("rootUrl"), QStringLiteral("file:///photos/phone")},
            {QStringLiteral("clockOffsetSeconds"), 0},
            {QStringLiteral("anchorCount"), 3},
        }};
    }
    [[nodiscard]] bool locationReferenceBusy() const noexcept { return false; }
    [[nodiscard]] QString locationReferenceErrorText() const { return {}; }
    Q_INVOKABLE void refreshLocationReferenceLibraries() { ++reference_refresh_count; }
    Q_INVOKABLE void addLocationReferenceLibrary(const QUrl&, const qlonglong) {}
    Q_INVOKABLE void removeLocationReferenceLibrary(const QString&) {}

    qlonglong start = -1;
    qlonglong end = -1;
    int request_count = 0;
    int reference_refresh_count = 0;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition)
        std::cerr << "Library location completion dialog contract failed: " << message << '\n';
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool invokeBool(QObject* const object, const char* const method) {
    QVariant returned;
    const bool invoked = QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, returned));
    drainBindings();
    return invoked && returned.toBool();
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.LibraryLocationCompletionDialogContract"),
        QStringLiteral("LibraryLocationCompletionDialog")
    );

    FakeLocationCompletionController controller;
    std::unique_ptr<QObject> dialog{component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
    })};
    if (!require(dialog != nullptr, "dialog should load from its packaged module")) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    bool valid = require(
        invokeBool(dialog.get(), "present"),
        "present should refresh the initial unbounded event query"
    );
    valid &= require(controller.request_count == 1 && controller.start == 0 && controller.end == 0,
                     "initial event lookup should remain unbounded");
    valid &= require(
        controller.reference_refresh_count == 1,
        "opening refreshes the evidence-only reference folder list"
    );

    dialog->setProperty("startDay", QStringLiteral("2026-08-01"));
    dialog->setProperty("endDay", QStringLiteral("2026-08-02"));
    valid &= require(invokeBool(dialog.get(), "refresh"), "valid capture dates should refresh");

    const auto expected_start = QDateTime(
        QDate{2026, 8, 1}, QTime{0, 0}, QTimeZone::systemTimeZone()
    ).toSecsSinceEpoch();
    const auto expected_end = QDateTime(
        QDate{2026, 8, 2}, QTime{23, 59, 59}, QTimeZone::systemTimeZone()
    ).toSecsSinceEpoch();
    valid &= require(
        controller.request_count == 2 && controller.start == expected_start && controller.end == expected_end,
        "the date range should use the local capture-day boundaries"
    );

    const QVariant groups = dialog->property("controller");
    valid &= require(groups.isValid(), "the controller property should remain bound");
    valid &= require(
        dialog->findChild<QObject*>(QStringLiteral("locationCompletionStartField")) != nullptr
            && dialog->findChild<QObject*>(QStringLiteral("locationCompletionEndField")) != nullptr
            && dialog->findChild<QObject*>(QStringLiteral("locationCompletionRefreshButton")) != nullptr,
        "the date-range controls should be reachable"
    );
    valid &= require(
        dialog->findChild<QObject*>(QStringLiteral("locationReferenceAddButton")) != nullptr
            && dialog->findChild<QObject*>(QStringLiteral("locationReferenceList")) != nullptr,
        "the reference-folder controls should be reachable"
    );
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "library_location_completion_dialog_test.moc"
