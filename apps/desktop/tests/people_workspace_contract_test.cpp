#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakePeopleAnalysisController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    Q_PROPERTY(uint analyzedPhotos READ analyzedPhotos NOTIFY changed)
    Q_PROPERTY(uint detectedFaces READ detectedFaces NOTIFY changed)
    Q_PROPERTY(uint embeddedFaces READ embeddedFaces NOTIFY changed)
    Q_PROPERTY(uint skippedItems READ skippedItems NOTIFY changed)
    Q_PROPERTY(uint ungroupedFaces READ ungroupedFaces NOTIFY changed)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)

  public:
    bool busy() const noexcept {
        return false;
    }
    bool hasResults() const noexcept {
        return has_results_;
    }
    QString statusText() const {
        return QStringLiteral("Ready");
    }
    QString errorText() const {
        return {};
    }
    QVariantList groups() const {
        return has_results_
            ? QVariantList{
                  QVariantMap{
                      {QStringLiteral("groupId"), QStringLiteral("a")},
                      {QStringLiteral("displayIndex"), 1},
                      {QStringLiteral("photoCount"), 3},
                  },
                  QVariantMap{
                      {QStringLiteral("groupId"), QStringLiteral("b")},
                      {QStringLiteral("displayIndex"), 2},
                      {QStringLiteral("photoCount"), 2},
                  },
              }
            : QVariantList{};
    }
    uint analyzedPhotos() const noexcept {
        return has_results_ ? 9U : 0U;
    }
    uint detectedFaces() const noexcept {
        return has_results_ ? 6U : 0U;
    }
    uint embeddedFaces() const noexcept {
        return has_results_ ? 5U : 0U;
    }
    uint skippedItems() const noexcept {
        return has_results_ ? 1U : 0U;
    }
    uint ungroupedFaces() const noexcept {
        return 0;
    }
    bool truncated() const noexcept {
        return false;
    }

    Q_INVOKABLE void startAnalysis() {
        ++start_count;
        has_results_ = true;
        emit changed();
    }
    Q_INVOKABLE void clearSessionResults() {
        ++clear_count;
        has_results_ = false;
        emit changed();
    }

    int start_count = 0;
    int clear_count = 0;

  signals:
    void changed();

  private:
    bool has_results_ = false;
};

namespace {

bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "People workspace contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void click(QQuickWindow& window, QQuickItem& item) {
    const QPointF position = item.mapToScene(QPointF{item.width() / 2.0, item.height() / 2.0});
    QMouseEvent press{
        QEvent::MouseButtonPress,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice(),
    };
    QGuiApplication::sendEvent(&window, &press);
    QMouseEvent release{
        QEvent::MouseButtonRelease,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice(),
    };
    QGuiApplication::sendEvent(&window, &release);
    drainBindings();
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.PeopleWorkspaceContract"),
        QStringLiteral("PeopleWorkspace")
    );
    FakePeopleAnalysisController controller;
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("width"), 900.0},
        {QStringLiteral("height"), 700.0},
    })};
    auto* const workspace = qobject_cast<QQuickItem*>(object.get());
    if (!workspace) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.setGeometry(0, 0, 900, 700);
    workspace->setParentItem(window.contentItem());
    window.show();
    drainBindings();

    auto* const start_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleStartButton"));
    if (!require(start_button != nullptr, "manual analysis action is packaged")) {
        return EXIT_FAILURE;
    }
    click(window, *start_button);
    if (!require(controller.start_count == 1, "manual action reaches the controller")
        || !require(
            workspace->property("renderedGroupCount").toInt() == 2,
            "anonymous group cards follow session results"
        )) {
        return EXIT_FAILURE;
    }

    const QString screenshot_path = qEnvironmentVariable("SHADOW_PEOPLE_UI_SCREENSHOT");
    if (!screenshot_path.isEmpty()
        && !require(window.grabWindow().save(screenshot_path), "visual snapshot is written")) {
        return EXIT_FAILURE;
    }

    auto* const clear_button =
        workspace->findChild<QQuickItem*>(QStringLiteral("peopleClearButton"));
    if (!require(clear_button != nullptr, "session clear action is packaged")) {
        return EXIT_FAILURE;
    }
    click(window, *clear_button);
    return require(controller.clear_count == 1, "clear action reaches the controller")
                   && require(
                       workspace->property("renderedGroupCount").toInt() == 0,
                       "clearing removes the session cards"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "people_workspace_contract_test.moc"
