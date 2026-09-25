#include <QGuiApplication>
#include <QDir>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    bool warnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& errors) {
        warnings = true;
        for (const auto& error : errors)
            std::cerr << error.toString().toStdString() << '\n';
    });
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import QtQuick.Controls
import Shadow.FormControlsContract
Item {
    id: fixture
    width: 420; height: 360
    property int activations: 0
    property int acceptedCount: 0
    property int rejectedCount: 0
    property bool dark: false
    onDarkChanged: Theme.effectiveDark = dark
    Component.onCompleted: Theme.effectiveDark = dark
    ShadowDialog {
        id: confirmation
        objectName: "confirmation"
        width: 360
        x: 20; y: 24
        title: "A themed confirmation"
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: fixture.acceptedCount++
        onRejected: fixture.rejectedCount++
        contentItem: Label { text: "Wrapped dialog content"; wrapMode: Text.WordWrap }
        function acceptItem() { return standardButton(Dialog.Ok) }
        function rejectItem() { return standardButton(Dialog.Cancel) }
    }
    ShadowTextField {
        objectName: "field"
        x: 12; y: 12; width: 260
        text: "300"
        validator: IntValidator { bottom: 1; top: 2400 }
    }
    ShadowComboBox {
        objectName: "combo"
        x: 12; y: 60; width: 260
        model: [{name: "sRGB"}, {name: "Display P3"}, {name: "测试色彩空间"}]
        textRole: "name"
        onActivated: parent.activations++
    }
    ShadowIcon {
        x: 12; y: 120; size: 40
        source: "qrc:/icons/map.svg"
        color: "#ff3030"
    }
    ShadowIcon {
        x: 72; y: 120; size: 40
        source: "qrc:/icons/settings.svg"
        color: "#ff3030"
    }
}
)QML", QUrl{});
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QQuickWindow window;
    window.resize(420, 360);
    qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
    window.show();
    auto* field = root->findChild<QQuickItem*>(QStringLiteral("field"));
    auto* combo = root->findChild<QQuickItem*>(QStringLiteral("combo"));
    bool ok = field && combo;
    auto check = [&](bool condition, const char* message) {
        if (!condition)
            std::cerr << message << '\n';
        ok &= condition;
    };
    if (!ok)
        return EXIT_FAILURE;
    // MultiEffect preserves source luminance. Every monochrome asset must
    // provide white RGB with alpha coverage, including assets not on screen.
    const QDir iconDirectory(QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/icons"));
    for (const QString& name : iconDirectory.entryList({QStringLiteral("*.svg")}, QDir::Files)) {
        const QImage icon(iconDirectory.filePath(name));
        check(!icon.isNull(), "a production SVG failed to rasterize");
        for (int y = 0; y < icon.height(); ++y)
            for (int x = 0; x < icon.width(); ++x) {
                const QColor pixel = icon.pixelColor(x, y);
                if (pixel.alpha() > 16 && (pixel.red() < 240 || pixel.green() < 240 || pixel.blue() < 240)) {
                    std::cerr << "icon contains untintable dark RGB: " << name.toStdString() << '\n';
                    ok = false;
                    y = icon.height();
                    break;
                }
            }
    }
    field->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier);
    for (const auto key : {Qt::Key_1, Qt::Key_2, Qt::Key_0, Qt::Key_0})
        QTest::keyClick(&window, key);
    check(field->property("text").toString() == QStringLiteral("1200")
              && field->property("acceptableInput").toBool(),
          "shared field lost editing or validator behavior");

    for (const bool dark : {false, true}) {
        root->setProperty("dark", dark);
        combo->forceActiveFocus();
        const int before = combo->property("currentIndex").toInt();
        const int count = root->property("activations").toInt();
        QTest::keyClick(&window, Qt::Key_Space);
        auto* popup = combo->property("popup").value<QObject*>();
        check(popup && QTest::qWaitFor([&] { return popup->property("visible").toBool(); }),
              "keyboard did not open shared combo");
        QTest::keyClick(&window, Qt::Key_Down);
        QTest::keyClick(&window, Qt::Key_Return);
        check(combo->property("currentIndex").toInt() == before + 1
                  && root->property("activations").toInt() == count + 1,
              "keyboard selection did not activate the object-model entry exactly once");
        QTest::keyClick(&window, Qt::Key_Space);
        QTest::keyClick(&window, Qt::Key_Up);
        QTest::keyClick(&window, Qt::Key_Escape);
        check(combo->property("currentIndex").toInt() == before + 1
                  && root->property("activations").toInt() == count + 1,
              "Escape committed a cancelled combo selection");
        check(field->height() == combo->height(), "field and combo heights drifted");
    }
    auto* confirmation = root->findChild<QObject*>(QStringLiteral("confirmation"));
    check(confirmation, "the packaged shared dialog is missing");
    if (confirmation) {
        for (const bool dark : {false, true}) {
            root->setProperty("dark", dark);
            const int accepted = root->property("acceptedCount").toInt();
            const int rejected = root->property("rejectedCount").toInt();
            QMetaObject::invokeMethod(confirmation, "open");
            check(QTest::qWaitFor([&] { return confirmation->property("opened").toBool(); }),
                  "shared confirmation did not open");
            QVariant button;
            QMetaObject::invokeMethod(confirmation, "acceptItem", Q_RETURN_ARG(QVariant, button));
            auto* accept = qobject_cast<QQuickItem*>(button.value<QObject*>());
            check(accept && accept->isVisible(), "the standard accept action disappeared");
            if (accept) {
                QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                                 accept->mapToScene(QPointF(accept->width() / 2, accept->height() / 2)).toPoint());
                check(root->property("acceptedCount").toInt() == accepted + 1,
                      "styled action must accept exactly once");
            }
            QMetaObject::invokeMethod(confirmation, "open");
            check(QTest::qWaitFor([&] { return confirmation->property("opened").toBool(); }),
                  "shared confirmation did not reopen");
            QTest::keyClick(&window, Qt::Key_Escape);
            check(root->property("rejectedCount").toInt() == rejected + 1,
                  "Escape must reject exactly once without acceptance");
            check(root->property("acceptedCount").toInt() == accepted + 1,
                  "rejecting a dialog accepted the action");
        }
    }
    QTest::qWait(150);
    const QImage frame = window.grabWindow();
    if (argc == 2)
        frame.save(QString::fromLocal8Bit(argv[1]));
    check(!frame.isNull(), "the shared-control render is empty");
    // The offscreen software adaptation does not execute ShaderEffect.
    // Its asset contract above still runs; native RHI runs also prove tint pixels.
    if (!frame.isNull() && window.rendererInterface()->graphicsApi() != QSGRendererInterface::Software) {
        const qreal scale = frame.devicePixelRatio();
        for (const int left : {12, 72}) {
            int tintedPixels = 0;
            for (int y = static_cast<int>(120 * scale); y < 160 * scale; ++y)
                for (int x = static_cast<int>(left * scale); x < (left + 40) * scale; ++x) {
                    const QColor pixel = frame.pixelColor(x, y);
                    tintedPixels += pixel.red() > 160 && pixel.green() < 100 && pixel.blue() < 100;
                }
            check(tintedPixels > 30, "monochrome SVGs must receive the requested tint");
        }
    }
    check(!warnings, "shared form controls produced QML warnings");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
