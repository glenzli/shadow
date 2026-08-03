#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QVariantList>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeTravelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList travelGroups READ travelGroups CONSTANT)
    Q_PROPERTY(qulonglong travelPhotoCount READ travelPhotoCount CONSTANT)
    Q_PROPERTY(bool travelCollectionsBusy READ travelCollectionsBusy CONSTANT)
    Q_PROPERTY(QString travelCollectionsErrorText READ travelCollectionsErrorText CONSTANT)

  public:
    [[nodiscard]] QVariantList travelGroups() const {
        return {
            QVariantMap{
                {QStringLiteral("key"), QStringLiteral("jp")},
                {QStringLiteral("label"), QStringLiteral("Japan")},
                {QStringLiteral("photoCount"), 3},
                {
                    QStringLiteral("destinations"),
                    QVariantList{QVariantMap{
                        {QStringLiteral("key"), QStringLiteral("jp\u001ftokyo\u001ftokyo")},
                        {QStringLiteral("label"), QStringLiteral("Tokyo, Tokyo, Japan")},
                        {QStringLiteral("photoCount"), 3},
                    }},
                },
            },
        };
    }
    [[nodiscard]] qulonglong travelPhotoCount() const noexcept {
        return 3;
    }
    [[nodiscard]] bool travelCollectionsBusy() const noexcept {
        return false;
    }
    [[nodiscard]] QString travelCollectionsErrorText() const {
        return {};
    }
};

class FakeProfile final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasLivingPlaces READ hasLivingPlaces CONSTANT)

  public:
    [[nodiscard]] bool hasLivingPlaces() const noexcept {
        return true;
    }
};

class FakeTravelWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* controller READ controller CONSTANT)
    Q_PROPERTY(QObject* personalProfile READ personalProfile CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)

  public:
    FakeTravelWorkspace(QObject* controller, QObject* profile) :
        controller_(controller), profile_(profile) {}
    [[nodiscard]] QObject* controller() const noexcept {
        return controller_;
    }
    [[nodiscard]] QObject* personalProfile() const noexcept {
        return profile_;
    }
    [[nodiscard]] QColor accent() const {
        return QColor(QStringLiteral("#2189d8"));
    }
    Q_INVOKABLE bool isTravelCollectionActive(const QString&, const QString&) const {
        return false;
    }
    Q_INVOKABLE void applyTravelCollection(const QString& country, const QString& locality) {
        applied_country = country;
        applied_locality = locality;
    }

    QString applied_country;
    QString applied_locality;

  private:
    QObject* controller_;
    QObject* profile_;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Travel collections QML contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    FakeTravelController controller;
    FakeProfile profile;
    FakeTravelWorkspace workspace(&controller, &profile);

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.TravelCollectionsContract"),
        QStringLiteral("ReviewTravelCollections")
    );
    std::unique_ptr<QObject> collections{component.createWithInitialProperties({
        {QStringLiteral("workspace"), QVariant::fromValue(&workspace)},
    })};
    if (!collections) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QObject* const all_travel =
        collections->findChild<QObject*>(QStringLiteral("allTravelCollectionMouse"));
    if (!require(
            collections->property("visible").toBool() && all_travel != nullptr,
            "configured living places must reveal the packaged Travel collection entry"
        )
        || !require(
            QMetaObject::invokeMethod(collections.get(), "activateAllTravel"),
            "the generated Travel entry must remain clickable"
        )) {
        return EXIT_FAILURE;
    }
    QCoreApplication::processEvents();
    return require(
               workspace.applied_country.isEmpty() && workspace.applied_locality.isEmpty(),
               "the root Travel entry must request the complete generated collection"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "review_travel_collections_test.moc"
