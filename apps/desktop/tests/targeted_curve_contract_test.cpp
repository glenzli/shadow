#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <iostream>
#include <memory>
class Tool final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active MEMBER active NOTIFY activeChanged)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(bool ready MEMBER ready NOTIFY changed)
    Q_PROPERTY(bool dragging MEMBER dragging NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
  public:
    bool active = true, busy = false, ready = true, dragging = false;
    int begun = 0, ended = 0, cancelled = 0, moves = 0;
    double x = 0, y = 0, delta = 0;
    QString status() const {
        return {};
    }
    Q_INVOKABLE void hover(double, double) {}
    Q_INVOKABLE void refresh() {}
    Q_INVOKABLE bool begin(double a, double b) {
        x = a;
        y = b;
        dragging = true;
        ++begun;
        emit changed();
        return true;
    }
    Q_INVOKABLE void move(double d) {
        delta = d;
        ++moves;
    }
    Q_INVOKABLE void finish(bool cancel = false) {
        if (dragging) {
            ++ended;
            if (cancel)
                ++cancelled;
        }
        dragging = false;
        emit changed();
    }
  signals:
    void activeChanged();
    void changed();
};
class Editor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* targetedCurve READ targetedCurve CONSTANT)
  public:
    Tool tool;
    QObject* targetedCurve() {
        return &tool;
    }
};
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    Editor editor;
    QQmlComponent component(
        &engine,
        QUrl("qrc:/qt/qml/Shadow/TargetedCurveContract/qml/PrecisionTargetedCurveOverlay.qml")
    );
    std::unique_ptr<QObject> object(
        component.createWithInitialProperties({{"editor", QVariant::fromValue<QObject*>(&editor)}})
    );
    if (!object)
        qWarning() << component.errors();
    require(bool(object), "packaged targeted overlay loads");
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QQuickWindow window;
    window.resize(500, 400);
    root->setParentItem(window.contentItem());
    root->setSize({500, 400});
    window.show();
    QTest::qWait(60);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {250, 200});
    require(
        editor.tool.begun == 1 && editor.tool.x == 0.5 && editor.tool.y == 0.5,
        "normalized source coordinates"
    );
    editor.tool.busy = true;
    editor.tool.ready = false;
    emit editor.tool.changed();
    QTest::mouseMove(&window, {250, 160});
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {250, 160});
    require(
        editor.tool.moves > 0 && std::abs(editor.tool.delta + 0.1) < 0.001 && editor.tool.ended == 1
            && editor.tool.cancelled == 0,
        "busy preview preserves active drag and release"
    );
    editor.tool.busy = false;
    editor.tool.ready = true;
    emit editor.tool.changed();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {250, 200});
    QTest::mouseMove(&window, {250, 170});
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {250, 170});
    require(
        editor.tool.cancelled == 1 && editor.tool.active,
        "Escape cancels gesture before exiting tool"
    );
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {250, 200});
    root->setProperty("interactionEnabled", false);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {250, 170});
    require(editor.tool.cancelled == 2, "comparison or workspace switch cancels gesture");
    return 0;
}
#include "targeted_curve_contract_test.moc"
