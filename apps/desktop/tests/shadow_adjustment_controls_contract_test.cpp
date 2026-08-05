#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTest>
#include <QUrl>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class AdjustmentSignalRecorder final : public QObject {
    Q_OBJECT

  public slots:
    void recordReset() {
        ++reset_count;
    }

    void recordEdited(const double value) {
        ++edited_count;
        edited_value = value;
    }

    void recordInlineReset(const double value) {
        ++inline_reset_count;
        inline_reset_value = value;
    }

    void recordGestureStarted() {
        ++gesture_started_count;
    }

    void recordGestureFinished() {
        ++gesture_finished_count;
    }

  public:
    int reset_count = 0;
    int edited_count = 0;
    int inline_reset_count = 0;
    int gesture_started_count = 0;
    int gesture_finished_count = 0;
    double edited_value = 0.0;
    double inline_reset_value = 0.0;
};

namespace {

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Shadow adjustment-controls contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::unique_ptr<QObject>
createSourceComponent(QQmlEngine& engine, const QString& file_name, const QVariantMap& properties) {
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/") + file_name)
    );
    std::unique_ptr<QObject> object(component.createWithInitialProperties(properties));
    if (!object) {
        std::cerr << component.errorString().toStdString();
    }
    return object;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    AdjustmentSignalRecorder recorder;

    auto section = createSourceComponent(
        engine,
        QStringLiteral("ShadowAdjustmentSection.qml"),
        {
            {QStringLiteral("title"), QStringLiteral("LIGHT")},
            {QStringLiteral("resetAvailable"), true},
            {QStringLiteral("resetObjectName"), QStringLiteral("panelResetButton")},
            {QStringLiteral("width"), 320.0},
        }
    );
    if (!section) {
        return EXIT_FAILURE;
    }
    QObject::connect(section.get(), SIGNAL(resetRequested()), &recorder, SLOT(recordReset()));
    QObject* const reset_button = section->findChild<QObject*>(QStringLiteral("panelResetButton"));
    if (!require(reset_button != nullptr, "reset button is exposed from the section header")
        || !require(
            QMetaObject::invokeMethod(reset_button, "clicked"),
            "header reset button can be activated"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(recorder.reset_count == 1, "header reset delegates exactly once")) {
        return EXIT_FAILURE;
    }

    auto slider = createSourceComponent(
        engine,
        QStringLiteral("ShadowSlider.qml"),
        {
            {QStringLiteral("label"), QStringLiteral("Exposure")},
            {QStringLiteral("from"), -2.0},
            {QStringLiteral("to"), 2.0},
            {QStringLiteral("neutralValue"), 0.0},
            {QStringLiteral("value"), 1.25},
            {QStringLiteral("width"), 320.0},
        }
    );
    if (!slider) {
        return EXIT_FAILURE;
    }
    QObject::connect(slider.get(), SIGNAL(edited(double)), &recorder, SLOT(recordEdited(double)));
    QObject::connect(
        slider.get(),
        SIGNAL(gestureStarted()),
        &recorder,
        SLOT(recordGestureStarted())
    );
    QObject::connect(
        slider.get(),
        SIGNAL(gestureFinished()),
        &recorder,
        SLOT(recordGestureFinished())
    );
    if (!require(
            QMetaObject::invokeMethod(slider.get(), "resetToNeutral"),
            "slider exposes its double-click neutralization action"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(recorder.edited_count == 1, "neutral reset emits exactly one edit")
        || !require(
            std::abs(recorder.edited_value) < 0.000'001,
            "neutral reset emits the declared neutral value"
        )
        || !require(
            recorder.gesture_started_count == 1 && recorder.gesture_finished_count == 1,
            "neutral reset brackets the edit as one undoable gesture"
        )) {
        return EXIT_FAILURE;
    }

    auto inline_slider = createSourceComponent(
        engine,
        QStringLiteral("ShadowInlineSlider.qml"),
        {
            {QStringLiteral("from"), 0.0},
            {QStringLiteral("to"), 100.0},
            {QStringLiteral("neutralValue"), 100.0},
            {QStringLiteral("fillFromMinimum"), true},
            {QStringLiteral("value"), 40.0},
            {QStringLiteral("width"), 240.0},
        }
    );
    if (!inline_slider) {
        return EXIT_FAILURE;
    }
    QObject::connect(
        inline_slider.get(),
        SIGNAL(resetRequested(double)),
        &recorder,
        SLOT(recordInlineReset(double))
    );
    if (!require(
            std::abs(inline_slider->property("fillStartPosition").toDouble()) < 0.000'001,
            "amount slider fill begins at its logical minimum"
        )
        || !require(
            std::abs(inline_slider->property("fillEndPosition").toDouble() - 0.4) < 0.000'001,
            "amount slider fill ends at its current value"
        )) {
        return EXIT_FAILURE;
    }

    auto* const inline_item = qobject_cast<QQuickItem*>(inline_slider.get());
    QQuickWindow interaction_window;
    interaction_window.resize(280, 80);
    inline_item->setParentItem(interaction_window.contentItem());
    inline_item->setPosition(QPointF{20.0, 24.0});
    interaction_window.show();
    drainBindings();
    auto* const handle =
        inline_slider->findChild<QQuickItem*>(QStringLiteral("shadowInlineSliderHandle"));
    if (!require(handle != nullptr, "inline slider exposes its visible thumb for interaction")) {
        return EXIT_FAILURE;
    }
    const QPointF initial_handle_center = handle->mapToItem(
        interaction_window.contentItem(),
        QPointF{handle->width() / 2.0, handle->height() / 2.0}
    );
    QTest::mousePress(
        &interaction_window,
        Qt::LeftButton,
        Qt::NoModifier,
        initial_handle_center.toPoint()
    );
    QTest::mouseMove(
        &interaction_window,
        (initial_handle_center + QPointF{72.0, 0.0}).toPoint(),
        40
    );
    QTest::mouseRelease(
        &interaction_window,
        Qt::LeftButton,
        Qt::NoModifier,
        (initial_handle_center + QPointF{72.0, 0.0}).toPoint()
    );
    drainBindings();
    if (!require(
            inline_slider->property("value").toDouble() > 65.0,
            "the visible thumb retains the Slider's real drag lifecycle"
        )) {
        return EXIT_FAILURE;
    }
    const QPointF dragged_handle_center = handle->mapToItem(
        interaction_window.contentItem(),
        QPointF{handle->width() / 2.0, handle->height() / 2.0}
    );
    QTest::mouseDClick(
        &interaction_window,
        Qt::LeftButton,
        Qt::NoModifier,
        dragged_handle_center.toPoint()
    );
    drainBindings();
    if (!require(
            recorder.inline_reset_count == 1
                && std::abs(recorder.inline_reset_value - 100.0) < 0.000'001,
            "a real thumb double click delegates its declared default exactly once"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "shadow_adjustment_controls_contract_test.moc"
