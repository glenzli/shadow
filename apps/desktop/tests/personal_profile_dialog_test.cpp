#include "personal_profile.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QVariantList>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeTravelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QVariantList livingPlaceCandidates READ livingPlaceCandidates NOTIFY
            travelCollectionsChanged
    )
    Q_PROPERTY(
        bool travelCollectionsBusy READ travelCollectionsBusy NOTIFY travelCollectionsChanged
    )

  public:
    [[nodiscard]] QVariantList livingPlaceCandidates() const {
        return {
            QVariantMap{
                {QStringLiteral("key"), QStringLiteral("cn\u001fshanghai\u001fshanghai")},
                {QStringLiteral("label"), QStringLiteral("Shanghai, Shanghai, China")},
            },
        };
    }
    [[nodiscard]] bool travelCollectionsBusy() const noexcept {
        return false;
    }
    Q_INVOKABLE void refreshTravelCollections() {
        ++refresh_count;
        emit travelCollectionsChanged();
    }

    int refresh_count = 0;

  signals:
    void travelCollectionsChanged();
};

class FakePersonalLocationSearch final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList results READ results NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString activeQuery READ activeQuery NOTIFY stateChanged)

  public:
    [[nodiscard]] QVariantList results() const {
        return {
            QVariantMap{
                {QStringLiteral("key"), QStringLiteral("cn\u001fbeijing\u001fbeijing")},
                {QStringLiteral("label"), QStringLiteral("Beijing · China")},
            },
        };
    }
    [[nodiscard]] bool busy() const noexcept {
        return false;
    }
    [[nodiscard]] QString errorText() const {
        return {};
    }
    [[nodiscard]] QString activeQuery() const {
        return QStringLiteral("beijing");
    }
    Q_INVOKABLE void search(const QString&) {
        ++search_count;
        emit stateChanged();
    }
    Q_INVOKABLE void clear() {
        ++clear_count;
        emit stateChanged();
    }

    int search_count = 0;
    int clear_count = 0;

  signals:
    void stateChanged();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Personal profile dialog contract failed: " << message << '\n';
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
    QTemporaryDir directory;
    if (!require(directory.isValid(), "the isolated profile directory must be available")) {
        return EXIT_FAILURE;
    }
    PersonalProfile profile(
        directory.path(),
        directory.filePath(QStringLiteral("preferences.ini"))
    );
    FakeTravelController controller;
    FakePersonalLocationSearch location_search;

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.PersonalProfileContract"),
        QStringLiteral("PersonalProfileDialog")
    );
    std::unique_ptr<QObject> dialog{component.createWithInitialProperties({
        {QStringLiteral("profile"), QVariant::fromValue(&profile)},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("locationSearch"), QVariant::fromValue(&location_search)},
        {QStringLiteral("hostWidth"), 1200.0},
        {QStringLiteral("hostHeight"), 800.0},
    })};
    if (!dialog) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    if (!require(
            QMetaObject::invokeMethod(dialog.get(), "present"),
            "the title-bar entry can present the profile dialog"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            controller.refresh_count == 1
                && dialog->findChild<QObject*>(QStringLiteral("personalProfileDoneButton"))
                && dialog->findChild<QObject*>(QStringLiteral("personalProfileNicknameField"))
                && dialog->findChild<QObject*>(QStringLiteral("personalProfileLocationSearchField"))
                && dialog->findChild<QObject*>(
                    QStringLiteral("personalProfileLocationLibraryCombo")
                ),
            "the packaged dialog exposes living-place search and Library controls"
        )) {
        return EXIT_FAILURE;
    }

    dialog->setProperty("nicknameDraft", QStringLiteral("Glendon"));
    QObject* const editor =
        dialog->findChild<QObject*>(QStringLiteral("personalProfileLivingPlacesEditor"));
    if (!require(
            editor && QMetaObject::invokeMethod(editor, "beginAdding"),
            "the compact presentation exposes a focused add-place action"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    QObject* const add_panel =
        dialog->findChild<QObject*>(QStringLiteral("personalProfileLivingPlaceAddPanel"));
    if (!require(
            add_panel && editor->property("adding").toBool(),
            "the add-place action enters the focused search state"
        )) {
        return EXIT_FAILURE;
    }
    QObject* const location_field =
        dialog->findChild<QObject*>(QStringLiteral("personalProfileLivingPlaceSearch"));
    if (!require(
            location_field
                && QMetaObject::invokeMethod(
                    location_field,
                    "chooseSearchResult",
                    Q_ARG(QVariant, QVariant::fromValue(0))
                ),
            "the manual-search result can add a canonical living place"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            editor->property("places").toList().size() == 1
                && editor->property("editingIndex").toInt() == 0,
            "choosing a result opens the new place in the focused editor"
        )) {
        return EXIT_FAILURE;
    }
    if (!require(
            dialog->findChild<QObject*>(
                QStringLiteral("personalProfileLivingPlaceTags")
            ) && dialog->findChild<QObject*>(QStringLiteral("personalProfileLivingPlaceEditPanel")),
            "the packaged dialog contains compact tag and edit-panel presentation owners"
        )) {
        return EXIT_FAILURE;
    }
    if (!require(
            QMetaObject::invokeMethod(
                editor,
                "replacePlace",
                Q_ARG(QVariant, QVariant::fromValue(0)),
                Q_ARG(QVariant, QVariant::fromValue(QStringLiteral("2020-03"))),
                Q_ARG(QVariant, QVariant::fromValue(QString()))
            ),
            "a selected place can become time-bounded"
        )) {
        return EXIT_FAILURE;
    }
    if (!require(
            QMetaObject::invokeMethod(dialog.get(), "saveAndClose"),
            "the Done action must persist one coherent profile draft"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    return require(
               profile.nickname() == QStringLiteral("Glendon") && profile.livingPlaces().size() == 1
                   && profile.livingPlaces()[0].toMap().value(QStringLiteral("key")).toString()
                          == QStringLiteral("cn\u001fbeijing\u001fbeijing")
                   && profile.livingPlaces()[0]
                              .toMap()
                              .value(QStringLiteral("startMonth"))
                              .toString()
                          == QStringLiteral("2020-03")
                   && location_search.clear_count >= 2,
               "nickname and living-place period must reach the local profile owner"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "personal_profile_dialog_test.moc"
