#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
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

    void onForegroundModeRequested(const bool foreground) {
        ++foreground_mode_count;
        last_foreground_mode = foreground;
    }

    void onRetryRequested() {
        ++retry_count;
    }

    void onApplyRequested() {
        ++apply_count;
    }

    void onCancelRequested() {
        ++cancel_count;
    }

  public:
    int point_count = 0;
    int undo_count = 0;
    int clear_count = 0;
    int foreground_mode_count = 0;
    int retry_count = 0;
    int apply_count = 0;
    int cancel_count = 0;
    double last_x = 0.0;
    double last_y = 0.0;
    bool last_foreground = false;
    bool last_foreground_mode = true;
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

[[nodiscard]] bool waitForProperty(
    QObject& object,
    const char* const property,
    const bool expected,
    const int timeout_ms = 1000
) {
    QElapsedTimer timer;
    timer.start();
    while (object.property(property).toBool() != expected && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
    }
    return object.property(property).toBool() == expected;
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
        {QStringLiteral("faceRegionMode"), false},
        {QStringLiteral("semanticMode"), false},
        {QStringLiteral("foregroundMode"), true},
        {QStringLiteral("promptPoints"), initial_points},
        {QStringLiteral("foregroundColor"), QColor{QStringLiteral("#73c48b")}},
        {QStringLiteral("backgroundColor"), QColor{QStringLiteral("#ef787d")}},
        {QStringLiteral("candidateColor"), QColor{QStringLiteral("#b18ae3")}},
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
    QObject::connect(
        overlay,
        SIGNAL(foregroundModeRequested(bool)),
        &recorder,
        SLOT(onForegroundModeRequested(bool))
    );
    QObject::connect(overlay, SIGNAL(retryRequested()), &recorder, SLOT(onRetryRequested()));
    QObject::connect(overlay, SIGNAL(applyRequested()), &recorder, SLOT(onApplyRequested()));
    QObject::connect(overlay, SIGNAL(cancelRequested()), &recorder, SLOT(onCancelRequested()));

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
            candidate != nullptr && !candidate->property("source").toUrl().isEmpty()
                && waitForProperty(*overlay, "candidateRendered", true)
                && candidate->property("opacity").toDouble() >= 0.5,
            "a staged candidate becomes a clearly visible colored overlay below the prompt points"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            overlay->property("guidanceText").toString().contains(QStringLiteral("preview")),
            "the canvas explains that the visible pixels are the current selection preview"
        )) {
        return EXIT_FAILURE;
    }

    auto* const action_dock = overlay->findChild<QQuickItem*>(QStringLiteral("aiMaskActionDock"));
    auto* const apply_button = overlay->findChild<QQuickItem*>(QStringLiteral("aiMaskApplyButton"));
    auto* const cancel_button =
        overlay->findChild<QQuickItem*>(QStringLiteral("aiMaskCancelButton"));
    if (!require(
            action_dock != nullptr && action_dock->isVisible() && apply_button != nullptr
                && apply_button->isVisible() && apply_button->isEnabled()
                && cancel_button != nullptr && cancel_button->isVisible()
                && cancel_button->isEnabled(),
            "a ready candidate keeps visible Apply and Cancel actions on the canvas"
        )) {
        return EXIT_FAILURE;
    }

    QMetaObject::invokeMethod(overlay, "requestForegroundMode", Q_ARG(QVariant, false));
    QMetaObject::invokeMethod(overlay, "requestApply");
    QMetaObject::invokeMethod(overlay, "requestCancel");
    if (!require(
            recorder.foreground_mode_count == 1 && !recorder.last_foreground_mode
                && recorder.apply_count == 1 && recorder.cancel_count == 1,
            "refinement and terminal actions remain controller-owned requests"
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
    if (!require(
            overlay->property("guidanceText").toString().contains(QStringLiteral("Updating")),
            "busy state is visible as an in-canvas selection update"
        )) {
        return EXIT_FAILURE;
    }
    click(window, QPointF{200.0, 100.0});
    QMetaObject::invokeMethod(overlay, "requestUndo");
    QMetaObject::invokeMethod(overlay, "requestApply");
    QMetaObject::invokeMethod(overlay, "requestCancel");
    if (!require(
            recorder.point_count == 2 && recorder.undo_count == 1 && recorder.apply_count == 1
                && recorder.cancel_count == 2,
            "busy generation freezes mutation and apply while keeping Cancel reachable"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("busy", false);
    overlay->setProperty("candidateVisible", false);
    overlay->setProperty("faceRegionMode", true);
    overlay->setProperty("peopleCount", 2);
    drainBindings();
    click(window, QPointF{200.0, 100.0});
    if (!require(
            recorder.point_count == 2 && overlay->property("renderedPointCount").toInt() == 0
                && overlay->property("guidanceText").toString().contains(QStringLiteral("person")),
            "people-detail mode uses the panel list and never asks for a canvas point"
        )) {
        return EXIT_FAILURE;
    }

    overlay->setProperty("faceRegionMode", false);
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
