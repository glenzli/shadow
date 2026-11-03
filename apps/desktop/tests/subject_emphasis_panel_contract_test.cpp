#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTranslator>
#include <memory>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTranslator chinese;
    if (!chinese.load(QStringLiteral(SHADOW_SUBJECT_TRANSLATION)))
        return 1;
    for (const bool translated : {false, true}) {
        if (translated)
            app.installTranslator(&chinese);
        QQmlEngine engine;
        bool warnings = false;
        QObject::connect(&engine, &QQmlEngine::warnings, [&](const auto&) { warnings = true; });
        QQmlComponent component(&engine);
        component.setData(
            R"QML(
import QtQuick
import Shadow.SubjectEmphasisContract
Item {
    width: 320; height: 480
    property QtObject assistant: QtObject {
        property bool active: true
        property bool applied: false
        property bool busy: false
        property bool applying: false
        property int nodes: 1
        property int historyEntries: 1
        property int dismissed: 0
        function dismiss() { active = false; applied = false; ++dismissed }
    }
    PrecisionSubjectEmphasisHeader {
        anchors.top: parent.top
        width: parent.width
        controller: parent.assistant
    }
}
)QML",
            QUrl{}
        );
        std::unique_ptr<QObject> root(component.create());
        if (!root)
            qFatal("%s", qPrintable(component.errorString()));
        QQuickWindow window;
        window.resize(320, 480);
        auto* item = qobject_cast<QQuickItem*>(root.get());
        item->setParentItem(window.contentItem());
        window.show();
        auto* assistant = root->property("assistant").value<QObject*>();
        auto* exit = root->findChild<QQuickItem*>(QStringLiteral("exitSubjectEmphasis"));
        if (!assistant || !exit || !QTest::qWaitFor([&] { return exit->isVisible(); }))
            return 2;
        const auto click = [&] {
            QTest::mouseClick(
                &window,
                Qt::LeftButton,
                Qt::NoModifier,
                exit->mapToScene(exit->boundingRect().center()).toPoint()
            );
            QCoreApplication::processEvents();
        };
        if (exit->property("text").toString()
            != (translated ? QStringLiteral("取消") : QStringLiteral("Cancel")))
            return 3;
        assistant->setProperty("applying", true);
        if (exit->isEnabled())
            return 4;
        assistant->setProperty("applying", false);
        click();
        if (assistant->property("dismissed").toInt() != 1 || exit->isVisible())
            return 5;
        assistant->setProperty("applied", true);
        if (!exit->isVisible()
            || exit->property("text").toString()
                   != (translated ? QStringLiteral("完成") : QStringLiteral("Done")))
            return 6;
        click();
        if (assistant->property("dismissed").toInt() != 2 || exit->isVisible()
            || assistant->property("nodes").toInt() != 1
            || assistant->property("historyEntries").toInt() != 1 || warnings)
            return 7;
    }
    return 0;
}
