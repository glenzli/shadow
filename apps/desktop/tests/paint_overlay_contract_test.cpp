#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <iostream>
#include <memory>

class Paint final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool strokeActive MEMBER active NOTIFY changed)
    Q_PROPERTY(bool canPaint MEMBER available NOTIFY changed)
    Q_PROPERTY(bool picking MEMBER picking NOTIFY brushChanged)
    Q_PROPERTY(bool erase MEMBER erase NOTIFY brushChanged)
    Q_PROPERTY(bool dodgeBurn MEMBER dodgeBurn CONSTANT)
    Q_PROPERTY(bool pressureSize MEMBER pressureSize CONSTANT)
  public:
    bool active = false, available = true, picking = false, erase = false;
    bool dodgeBurn = false, pressureSize = false;
    int begun = 0, ended = 0, cancelled = 0, moves = 0;
    Q_INVOKABLE QVariantMap cursorShape(double, double, double) {
        return {{"ux", .03}, {"uy", 0}, {"vx", 0}, {"vy", .03}};
    }
    Q_INVOKABLE bool beginStroke(double, double, double, double, bool) {
        active = true;
        ++begun;
        emit changed();
        return true;
    }
    Q_INVOKABLE void appendPoint(double, double, double) {
        ++moves;
    }
    Q_INVOKABLE void finishStroke() {
        if (active)
            ++ended;
        active = false;
        emit changed();
    }
    Q_INVOKABLE void cancelStroke() {
        if (active)
            ++cancelled;
        active = false;
        emit changed();
    }
  signals:
    void changed();
    void brushChanged();
};
class Editor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* paint READ paint CONSTANT)
    Q_PROPERTY(QString previewSource MEMBER source NOTIFY sourceChanged)
  public:
    Paint tool;
    QString source = "image://test/current";
    QObject* paint() {
        return &tool;
    }
  signals:
    void parametersChanged();
    void sourceChanged();
};
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    Editor editor;
    QQmlComponent component(
        &engine,
        QUrl("qrc:/qt/qml/Shadow/PaintContract/qml/PrecisionPaintOverlay.qml")
    );
    std::unique_ptr<QObject> object(component.createWithInitialProperties(
        {{"editor", QVariant::fromValue<QObject*>(&editor)},
         {"interactionEnabled", true},
         {"previewReady", true},
         {"previewGeneration", "1"},
         {"outputAspectRatio", 1.5}}
    ));
    if (!object)
        qWarning() << component.errors();
    require(bool(object), "packaged paint overlay loads");
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QQuickWindow window;
    window.resize(600, 400);
    root->setParentItem(window.contentItem());
    root->setSize({600, 400});
    window.show();
    QTest::qWait(50);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {250, 200});
    root->setProperty("previewReady", false);
    QTest::mouseMove(&window, {300, 200});
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {300, 200});
    require(
        editor.tool.ended == 1 && editor.tool.cancelled == 0,
        "preview replacement retains active capture"
    );
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {300, 250});
    require(editor.tool.ended == 2, "next stroke starts while the replacement texture is loading");
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {250, 200});
    root->setProperty("interactionEnabled", false);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {300, 200});
    require(
        editor.tool.cancelled == 1 && editor.tool.ended == 2,
        "leaving tool cancels exactly once"
    );
    editor.source.clear();
    emit editor.sourceChanged();
    root->setProperty("interactionEnabled", true);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {300, 250});
    require(editor.tool.begun == 3, "unprepared photo cannot receive ink");
    editor.source = "image://test/next";
    editor.tool.available = false;
    emit editor.sourceChanged();
    emit editor.tool.changed();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, {300, 250});
    require(editor.tool.begun == 3, "locked photo or disabled layer cannot receive ink");
    return 0;
}
#include "paint_overlay_contract_test.moc"
