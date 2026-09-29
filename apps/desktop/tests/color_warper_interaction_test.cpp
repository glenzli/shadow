#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTranslator>
#include <array>
#include <cmath>
#include <memory>

namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        qCritical() << message;
        std::exit(1);
    }
}
class Editor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool gradeNodeEnabled READ active CONSTANT)
    Q_PROPERTY(int parameterRevision MEMBER revision NOTIFY changed)
    Q_PROPERTY(QVariantList colorWarperControlPoints READ points NOTIFY changed)
  public:
    bool active() const {
        return true;
    }
    std::array<QPointF, 25> values{};
    int revision = 0, writes = 0, begins = 0, ends = 0;
    QString activeKey;
    QVariantList points() const {
        QVariantList result;
        for (int i = 0; i < 25; ++i)
            result.append(
                QVariantMap{
                    {"row", i / 5},
                    {"column", i % 5},
                    {"aOffset", values[i].x()},
                    {"bOffset", values[i].y()}
                }
            );
        return result;
    }
    Q_INVOKABLE double parameterValue(const QString&) const {
        return 1;
    }
    Q_INVOKABLE void setParameterValue(const QString&, double) {}
    Q_INVOKABLE void beginParameterEdit(const QString& key) {
        check(activeKey.isEmpty(), "gestures must not overlap");
        activeKey = key;
        ++begins;
    }
    Q_INVOKABLE void endParameterEdit(const QString& key) {
        check(activeKey == key, "gesture ends at the same owner");
        activeKey.clear();
        ++ends;
    }
    Q_INVOKABLE void setColorWarperControlPoint(int index, double a, double b) {
        check(
            index >= 0 && index < 25 && std::abs(a) <= .32 && std::abs(b) <= .32,
            "bounded point coordinates"
        );
        values[index] = {a, b};
        ++writes;
        ++revision;
        emit changed();
    }
    Q_INVOKABLE void resetColorWarper() {
        values = {};
        ++revision;
        emit changed();
    }
  signals:
    void changed();
    void selectedGradeNodeChanged();
    void sourceIdentityChanged();
};
QPoint center(QQuickItem* item) {
    return item->mapToScene(item->boundingRect().center()).toPoint();
}
void move(QQuickWindow& window, QPoint position, Qt::KeyboardModifiers modifiers = {}) {
    QMouseEvent event(
        QEvent::MouseMove,
        position,
        window.mapToGlobal(position),
        Qt::NoButton,
        Qt::LeftButton,
        modifiers
    );
    QCoreApplication::sendEvent(&window, &event);
    QTest::qWait(10);
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTranslator chinese;
    check(chinese.load(QStringLiteral(SHADOW_WARPER_TRANSLATION)), "Chinese catalog loads");
    for (bool translated : {false, true}) {
        if (translated)
            app.installTranslator(&chinese);
        QQmlEngine engine;
        Editor editor;
        QQmlComponent component(
            &engine,
            QUrl("qrc:/qt/qml/Shadow/WarperContract/qml/PrecisionColorWarperPanel.qml")
        );
        std::unique_ptr<QObject> object(component.createWithInitialProperties(
            {{"editor", QVariant::fromValue<QObject*>(&editor)}}
        ));
        if (!object)
            qCritical() << component.errors();
        check(bool(object), "packaged expanded panel loads");
        auto* panel = qobject_cast<QQuickItem*>(object.get());
        auto* grid = panel->findChild<QQuickItem*>("expandedColorWarperEditor");
        auto* frame = grid->findChild<QQuickItem*>("colorWarperFrame");
        check(grid && frame, "shared grid reachable");
        QQuickWindow window;
        window.resize(520, 800);
        panel->setParentItem(window.contentItem());
        panel->setSize({520, 800});
        window.show();
        const bool gridReady = QTest::qWaitFor([&] { return frame->width() > 400; }, 2000);
        if (!gridReady)
            qCritical() << "Panel/grid geometry" << panel->size() << grid->size() << frame->size()
                        << grid->property("availableMeshHeight") << window.size();
        check(gridReady, "expanded grid has useful precision space");
        auto* resizeHandle = panel->findChild<QQuickItem*>("colorWarperResizeHandle");
        QSignalSpy resizeRequests(panel, SIGNAL(widthRequested(double)));
        const auto resizeStart = resizeHandle->mapToScene({3, 250}).toPoint();
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, resizeStart);
        move(window, resizeStart + QPoint(20, 0));
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, resizeStart + QPoint(20, 0));
        check(
            resizeRequests.isValid() && !resizeRequests.empty()
                && std::abs(resizeRequests.last().front().toDouble() - 500) < 1,
            "divider pointer drag requests the exact panel width"
        );
        const QPoint start = center(frame) + QPoint(8, 6);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, start);
        check(
            editor.writes == 0 && editor.values[12].isNull(),
            "selection must not jump the point"
        );
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        move(window, start + QPoint(4, 0));
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, start + QPoint(4, 0));
        const double normal = editor.values[12].x();
        check(
            normal > .005 && std::abs(editor.values[12].y()) < 1e-9,
            "drag retains the original grab offset"
        );
        editor.resetColorWarper();
        QTest::mousePress(&window, Qt::LeftButton, Qt::ShiftModifier, start);
        move(window, start + QPoint(4, 0), Qt::ShiftModifier);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::ShiftModifier, start + QPoint(4, 0));
        check(
            std::abs(editor.values[12].x() - normal * .1) < 1e-6,
            "Shift drag is ten times finer"
        );
        editor.resetColorWarper();
        grid->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Right);
        QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
        check(
            std::abs(editor.values[12].x() - .002) < 1e-8
                && std::abs(editor.values[12].y() - .0002) < 1e-8,
            "keyboard fine steps preserve both axes"
        );
        const int beforeBegins = editor.begins;
        QTest::keyPress(&window, Qt::Key_Right);
        QTest::keyPress(&window, Qt::Key_Right);
        QTest::keyRelease(&window, Qt::Key_Right);
        check(
            editor.begins == beforeBegins + 1 && editor.ends == editor.begins,
            "repeated keyboard steps form one gesture"
        );
        auto* axis = grid->findChild<QQuickItem*>("colorWarperAxisA");
        auto* valueLabel = axis->findChild<QQuickItem*>("shadowSliderValueLabel");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(valueLabel));
        for (const auto character : QStringLiteral("0.0123"))
            QTest::keyClick(&window, static_cast<Qt::Key>(character.unicode()));
        QTest::keyClick(&window, Qt::Key_Return);
        check(
            std::abs(editor.values[12].x() - .0123) < 1e-8,
            "numeric entry authors the exact selected-axis value"
        );
        QTest::mouseClick(
            &window,
            Qt::LeftButton,
            Qt::NoModifier,
            center(grid->findChild<QQuickItem*>("colorWarperResetPoint"))
        );
        check(editor.values[12].isNull(), "selected-point reset works");
        grid->forceActiveFocus();
        QTest::keyPress(&window, Qt::Key_Right);
        panel->setVisible(false);
        QTest::keyRelease(&window, Qt::Key_Right);
        check(
            editor.activeKey.isEmpty() && editor.begins == editor.ends,
            "hiding the panel retires a held keyboard gesture"
        );
        panel->setVisible(true);
        for (int width : {340, 520, 640}) {
            window.resize(width, 800);
            panel->setWidth(width);
            QTest::qWait(30);
            check(
                frame->width() <= width - 32 && frame->width() >= 280,
                "resizing preserves a bounded usable grid"
            );
        }
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(valueLabel));
        for (const auto character : QStringLiteral("0.2000"))
            QTest::keyClick(&window, static_cast<Qt::Key>(character.unicode()));
        const auto beforeSourceChange = editor.values;
        emit editor.sourceIdentityChanged();
        check(
            editor.values == beforeSourceChange && !axis->property("valueEditing").toBool(),
            "uncommitted numeric text must not follow a new photo"
        );
        check(grid->property("selectedPoint").toInt() == -1, "selection is photo-local");
        object.reset();
        if (translated)
            app.removeTranslator(&chinese);
    }
    qInfo() << "Color map: no-jump drag, Shift precision, keyboard, numeric input, reset, "
               "lifecycle, resizing and bilingual UI passed";
}
#include "color_warper_interaction_test.moc"
