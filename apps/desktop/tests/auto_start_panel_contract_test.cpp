#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTranslator>
#include <cstdlib>
#include <memory>
namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        qCritical() << message;
        std::exit(1);
    }
}
void bounds(QQuickItem* item, QQuickItem* panel) {
    if (!item->isVisible())
        return;
    const auto r = item->mapRectToItem(panel, item->boundingRect());
    if (r.width() > 0 && (r.left() < -3 || r.right() > panel->width() + 3)) {
        qCritical() << item << r << panel->width();
        check(false, "popup content overflow");
    }
    for (auto* child : item->childItems())
        bounds(child, panel);
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTranslator chinese;
    check(chinese.load(QStringLiteral(SHADOW_AUTO_START_TRANSLATION)), "Chinese translation loads");
    for (bool translated : {false, true}) {
        if (translated)
            app.installTranslator(&chinese);
        for (bool dark : {false, true}) {
            QQmlEngine engine;
            QQmlComponent fake(&engine);
            fake.setData(
                R"(
import QtQml
QtObject {
 signal sourceIdentityChanged()
 property bool active: true
 property bool stateBusy: false
 property QtObject autoStart: QtObject {
  property bool active: false
  property bool busy: false
  property bool ready: false
  property bool applying: false
  property bool canApply: false
  property bool whiteBalanceEnabled: true
  property bool toneEnabled: true
  property bool skinEnabled: true
  property bool whiteBalanceAvailable: true
  property bool toneAvailable: true
  property bool skinAvailable: true
  property bool automaticEnabled: false
  property real strength: 1
  property string status: "Local AI is checking scene lighting and skin. This is a deliberately long status line for a small editor window."
  property string summary: "White balance: a small correction.\nTone: +0.30 EV.\nSkin: preserve original colour."
  property string previewSource: ""
  property string originalSource: ""
  property int analysisCount: 0
  property int applyCount: 0
  signal applied()
  function analyze() { active = true; busy = true; ++analysisCount }
  function cancel() { active = false; busy = false }
  function apply() { ++applyCount; active = false; applied() }
 }
})",
                QUrl()
            );
            std::unique_ptr<QObject> editor(fake.create());
            check(bool(editor), "fake editor creates");
            QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                    "qrc:/qt/qml/Shadow/AutoStartContract/qml/PrecisionAutoStart.qml"
                ))
            );
            if (component.isError())
                qCritical() << component.errors();
            std::unique_ptr<QObject> object(component.createWithInitialProperties(
                {{"editor", QVariant::fromValue(editor.get())}}
            ));
            check(bool(object), "packaged automatic start component creates");
            auto* theme = engine.singletonInstance<QObject*>(
                qmlTypeId("Shadow.AutoStartContract", 1, 0, "Theme")
            );
            check(theme, "shared theme available");
            theme->setProperty("mode", dark ? 2 : 1);
            auto* root = qobject_cast<QQuickItem*>(object.get());
            auto* feature = editor->property("autoStart").value<QObject*>();
            const auto preview = qEnvironmentVariable("SHADOW_AUTO_UI_PREVIEW");
            if (!preview.isEmpty()) {
                feature->setProperty("previewSource", QUrl::fromLocalFile(preview).toString());
                feature->setProperty("originalSource", QUrl::fromLocalFile(preview).toString());
            }
            QQuickWindow window;
            root->setParentItem(window.contentItem());
            root->setSize(QSizeF(140, 32));
            window.show();
            auto* entry = root->findChild<QQuickItem*>("autoStartEntry");
            auto* dialog = root->findChild<QObject*>("autoStartDialog");
            check(entry && dialog, "entry and popup registered");
            check(
                entry->property("text").toString()
                    == (translated ? QString::fromUtf8("自动") : QStringLiteral("Auto")),
                "entry uses the short product name"
            );
            for (int width : {400, 640, 1100}) {
                window.resize(width, 650);
                QTest::qWait(30);
                QTest::mouseClick(
                    &window,
                    Qt::LeftButton,
                    Qt::NoModifier,
                    entry->mapToScene(entry->boundingRect().center()).toPoint()
                );
                QTest::qWait(30);
                check(dialog->property("visible").toBool(), "pointer opens popup");
                const auto entrySize = entry->size();
                for (bool busy : {true, false}) {
                    feature->setProperty("busy", busy);
                    feature->setProperty("ready", !busy);
                    feature->setProperty("canApply", !busy);
                    QTest::qWait(30);
                    check(entry->size() == entrySize, "analysis keeps entry frame stable");
                    auto* content = dialog->property("contentItem").value<QQuickItem*>();
                    check(content, "popup content exists");
                    bounds(content, content);
                    check(dialog->property("width").toDouble() <= width - 32, "popup fits window");
                }
                const auto evidence = qEnvironmentVariable("SHADOW_AUTO_UI_EVIDENCE");
                if (width == 640 && !evidence.isEmpty()) {
                    feature->setProperty(
                        "status",
                        QCoreApplication::translate(
                            "EditAutoStartController",
                            "Local AI suggestion ready · %1. Review before applying."
                        )
                            .arg(QStringLiteral("Qwen3-VL"))
                    );
                    feature->setProperty(
                        "summary",
                        QCoreApplication::translate(
                            "EditAutoStartController",
                            "White balance: a small correction around the current white point."
                        ) + QLatin1Char('\n')
                            + QCoreApplication::translate(
                                  "EditAutoStartController",
                                  "Tone: %1 EV, gentle highlight and shadow balance."
                            )
                                  .arg("0.00")
                            + QLatin1Char('\n')
                            + QCoreApplication::translate(
                                "EditAutoStartController",
                                "Skin: subtle hue uniformity for %n person(s), preserving each "
                                "person's colour.",
                                nullptr,
                                1
                            )
                    );
                    QTest::qWait(80);
                    check(
                        window.grabWindow().save(QDir(evidence).filePath(
                            QStringLiteral("auto-%1-%2.png")
                                .arg(translated ? "zh" : "en", dark ? "dark" : "light")
                        )),
                        "dialog screenshot saved"
                    );
                }
                QMetaObject::invokeMethod(dialog, "close");
            }
            window.resize(640, 650);
            QMetaObject::invokeMethod(dialog, "open");
            QTest::qWait(20);
            QMetaObject::invokeMethod(editor.get(), "sourceIdentityChanged");
            QTest::qWait(20);
            check(
                !dialog->property("visible").toBool(),
                "changing photo dismisses the old suggestion"
            );
            QMetaObject::invokeMethod(dialog, "open");
            QTest::qWait(20);
            for (const auto& option :
                 {std::pair{"autoStartWhiteBalance", "whiteBalanceEnabled"},
                  std::pair{"autoStartTone", "toneEnabled"},
                  std::pair{"autoStartSkin", "skinEnabled"},
                  std::pair{"autoStartAutomatic", "automaticEnabled"}}) {
                auto* control = root->findChild<QQuickItem*>(option.first);
                check(
                    control && control->property("shadowStyled").toBool(),
                    "options use shared Shadow controls"
                );
                const bool before = feature->property(option.second).toBool();
                QTest::mouseClick(
                    &window,
                    Qt::LeftButton,
                    Qt::NoModifier,
                    control->mapToScene(control->boundingRect().center()).toPoint()
                );
                QTest::qWait(20);
                check(
                    feature->property(option.second).toBool() != before,
                    "shared option forwards pointer changes"
                );
            }
            auto* apply = root->findChild<QQuickItem*>("autoStartApply");
            check(apply && apply->isEnabled(), "apply action reachable");
            QTest::mouseClick(
                &window,
                Qt::LeftButton,
                Qt::NoModifier,
                apply->mapToScene(apply->boundingRect().center()).toPoint()
            );
            QTest::qWait(30);
            check(
                feature->property("applyCount").toInt() == 1
                    && !dialog->property("visible").toBool(),
                "apply closes successful proposal exactly once"
            );
            object.reset();
        }
        if (translated)
            app.removeTranslator(&chinese);
    }
    qInfo() << "Auto dialog: English/Chinese, light/dark, narrow/wide, busy/ready geometry and "
               "pointer actions passed";
}
