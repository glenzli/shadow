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
        QVariantList travelHomeCandidates READ travelHomeCandidates NOTIFY travelCollectionsChanged
    )
    Q_PROPERTY(
        bool travelCollectionsBusy READ travelCollectionsBusy NOTIFY travelCollectionsChanged
    )

  public:
    [[nodiscard]] QVariantList travelHomeCandidates() const {
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

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.PersonalProfileContract"),
        QStringLiteral("PersonalProfileDialog")
    );
    std::unique_ptr<QObject> dialog{component.createWithInitialProperties({
        {QStringLiteral("profile"), QVariant::fromValue(&profile)},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
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
                && dialog->findChild<QObject*>(QStringLiteral("personalProfileHomeCombo")),
            "the packaged dialog exposes its profile controls and refreshes Library places"
        )) {
        return EXIT_FAILURE;
    }

    dialog->setProperty("nicknameDraft", QStringLiteral("Glendon"));
    dialog->setProperty("homeKeyDraft", QStringLiteral("cn\u001fshanghai\u001fshanghai"));
    dialog->setProperty("homeLabelDraft", QStringLiteral("Shanghai, Shanghai, China"));
    if (!require(
            QMetaObject::invokeMethod(dialog.get(), "saveAndClose"),
            "the Done action must persist one coherent profile draft"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    return require(
               profile.nickname() == QStringLiteral("Glendon")
                   && profile.homeLocalityKey() == QStringLiteral("cn\u001fshanghai\u001fshanghai"),
               "nickname and structured home locality must reach the local profile owner"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "personal_profile_dialog_test.moc"
