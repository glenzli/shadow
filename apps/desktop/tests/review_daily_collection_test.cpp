#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeDailyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(qulonglong dailyPhotoCount READ dailyPhotoCount CONSTANT)
    Q_PROPERTY(bool travelCollectionsBusy READ travelCollectionsBusy CONSTANT)

  public:
    [[nodiscard]] qulonglong dailyPhotoCount() const noexcept {
        return 7;
    }
    [[nodiscard]] bool travelCollectionsBusy() const noexcept {
        return false;
    }
};

class FakeDailyProfile final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasLivingPlaces READ hasLivingPlaces CONSTANT)

  public:
    [[nodiscard]] bool hasLivingPlaces() const noexcept {
        return true;
    }
};

class FakeDailyWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* controller READ controller CONSTANT)
    Q_PROPERTY(QObject* personalProfile READ personalProfile CONSTANT)

  public:
    FakeDailyWorkspace(QObject* controller, QObject* profile) :
        controller_(controller), profile_(profile) {}
    [[nodiscard]] QObject* controller() const noexcept {
        return controller_;
    }
    [[nodiscard]] QObject* personalProfile() const noexcept {
        return profile_;
    }
    Q_INVOKABLE bool isDailyCollectionActive() const {
        return false;
    }
    Q_INVOKABLE void applyDailyCollection() {
        applied = true;
    }

    bool applied = false;

  private:
    QObject* controller_;
    QObject* profile_;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Daily collection QML contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    FakeDailyController controller;
    FakeDailyProfile profile;
    FakeDailyWorkspace workspace(&controller, &profile);

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.DailyCollectionContract"),
        QStringLiteral("ReviewDailyCollection")
    );
    std::unique_ptr<QObject> collection{component.createWithInitialProperties({
        {QStringLiteral("workspace"), QVariant::fromValue(&workspace)},
    })};
    if (!collection) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QObject* const daily = collection->findChild<QObject*>(QStringLiteral("dailyCollectionMouse"));
    if (!require(
            collection->property("visible").toBool() && daily != nullptr,
            "configured living places must reveal the packaged Daily collection entry"
        )
        || !require(
            QMetaObject::invokeMethod(collection.get(), "activateDaily"),
            "the generated Daily entry must remain clickable"
        )) {
        return EXIT_FAILURE;
    }
    QCoreApplication::processEvents();
    return require(workspace.applied, "the Daily entry must request the ordinary-life collection")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "review_daily_collection_test.moc"
