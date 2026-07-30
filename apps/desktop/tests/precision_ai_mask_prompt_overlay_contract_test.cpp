#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class PromptRecorder final : public QObject {
    Q_OBJECT

  public slots:
    void onPointRequested(const double x, const double y, const bool foreground) {
        ++point_count;
        last_x = x;
        last_y = y;
        last_foreground = foreground;
    }

    void onUndoRequested() {
        ++undo_count;
    }

    void onClearRequested() {
        ++clear_count;
    }

  public:
    int point_count = 0;
    int undo_count = 0;
    int clear_count = 0;
    double last_x = 0.0;
    double last_y = 0.0;
    bool last_foreground = false;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision AI-mask prompt contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void click(QQuickWindow& window, const QPointF& position) {
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
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    if (argc > 1) {
        component.loadUrl(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    } else {
        component.loadFromModule(
            QStringLiteral("Shadow.AiMaskPromptOverlayContract"),
            QStringLiteral("PrecisionAiMaskPromptOverlay")
        );
    }

    const QVariantList initial_points{
        QVariantMap{
            {QStringLiteral("x"), 0.25},
            {QStringLiteral("y"), 0.75},
            {QStringLiteral("foreground"), true},
        },
        QVariantMap{
            {QStringLiteral("x"), 0.75},
            {QStringLiteral("y"), 0.25},
            {QStringLiteral("foreground"), false},
        },
    };
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("interactionEnabled"), true},
        {QStringLiteral("busy"), false},
        {QStringLiteral("foregroundMode"), true},
        {QStringLiteral("promptPoints"), initial_points},
        {QStringLiteral("foregroundColor"), QColor{QStringLiteral("#73c48b")}},
        {QStringLiteral("backgroundColor"), QColor{QStringLiteral("#ef787d")}},
        {QStringLiteral("candidateSource"), QString{}},
        {QStringLiteral("candidateVisible"), false},
        {QStringLiteral("width"), 400.0},
        {QStringLiteral("height"), 200.0},
    })};
    auto* const overlay = qobject_cast<QQuickItem*>(object.get());
    if (!overlay) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    PromptRecorder recorder;
    QObject::connect(
        overlay,
        SIGNAL(pointRequested(double, double, bool)),
        &recorder,
        SLOT(onPointRequested(double, double, bool))
    );
    QObject::connect(overlay, SIGNAL(undoRequested()), &recorder, SLOT(onUndoRequested()));
    QObject::connect(overlay, SIGNAL(clearRequested()), &recorder, SLOT(onClearRequested()));

    QQuickWindow window;
    window.setGeometry(0, 0, 400, 200);
    overlay->setParentItem(window.contentItem());
    window.show();
    drainBindings();

    if (!require(
            overlay->property("pointCount").toInt() == 2
                && overlay->property("renderedPointCount").toInt() == 2,
            "accepted foreground and background points are rendered"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("candidateVisible", true);
    overlay->setProperty(
        "candidateSource",
        QStringLiteral(
            "data:image/png;base64,"
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
            "YAAAAAYAAjCB0C8AAAAASUVORK5CYII="
        )
    );
    drainBindings();
    auto* const candidate = overlay->findChild<QQuickItem*>(QStringLiteral("aiMaskCandidateImage"));
    if (!require(
            candidate != nullptr && !candidate->property("source").toUrl().isEmpty(),
            "a staged candidate source is projected below the prompt points"
        )) {
        return EXIT_FAILURE;
    }

    click(window, QPointF{100.0, 50.0});
    if (!require(
            recorder.point_count == 1 && std::abs(recorder.last_x - 0.25) < 0.0001
                && std::abs(recorder.last_y - 0.25) < 0.0001 && recorder.last_foreground,
            "foreground click emits clamped normalized coordinates"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("foregroundMode", false);
    click(window, QPointF{320.0, 120.0});
    if (!require(
            recorder.point_count == 2 && std::abs(recorder.last_x - 0.8) < 0.0001
                && std::abs(recorder.last_y - 0.6) < 0.0001 && !recorder.last_foreground,
            "exclude mode emits one background prompt point"
        )) {
        return EXIT_FAILURE;
    }

    QMetaObject::invokeMethod(overlay, "requestUndo");
    QMetaObject::invokeMethod(overlay, "requestClear");
    if (!require(
            recorder.undo_count == 1 && recorder.clear_count == 1,
            "undo and clear are explicit controller-owned requests"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("busy", true);
    click(window, QPointF{200.0, 100.0});
    QMetaObject::invokeMethod(overlay, "requestUndo");
    if (!require(
            recorder.point_count == 2 && recorder.undo_count == 1,
            "busy generation freezes point mutation"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("busy", false);
    overlay->setProperty("maximumPoints", 2);
    click(window, QPointF{200.0, 100.0});
    return require(
               recorder.point_count == 2,
               "the bounded provider point limit disables further clicks"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "precision_ai_mask_prompt_overlay_contract_test.moc"
