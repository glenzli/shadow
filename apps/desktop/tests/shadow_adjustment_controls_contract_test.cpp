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
            std::abs(slider->property("value").toDouble()) < 0.000'001,
            "neutral reset synchronizes the visible slider before model feedback"
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
            inline_slider->property("fillFromMinimum").toBool(),
            "positive maximum-default amounts fill from their logical minimum by default"
        )
        || !require(
            std::abs(inline_slider->property("fillStartPosition").toDouble()) < 0.000'001,
            "amount slider fill begins at its logical minimum"
        )
        || !require(
            std::abs(inline_slider->property("fillEndPosition").toDouble() - 0.4) < 0.000'001,
            "amount slider fill ends at its current value"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            !slider->property("fillFromMinimum").toBool(),
            "signed adjustments continue to visualize distance from neutral"
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
    QTest::mouseClick(
        &interaction_window,
        Qt::LeftButton,
        Qt::NoModifier,
        initial_handle_center.toPoint()
    );
    drainBindings();
    if (!require(
            std::abs(inline_slider->property("value").toDouble() - 40.0) < 0.000'001,
            "a thumb click waits for drag or double click instead of starting an edit"
        )) {
        return EXIT_FAILURE;
    }
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

    auto keyboard_slider = createSourceComponent(
        engine,
        QStringLiteral("ShadowSlider.qml"),
        {
            {QStringLiteral("label"), QStringLiteral("Exposure")},
            {QStringLiteral("from"), -2.0},
            {QStringLiteral("to"), 2.0},
            {QStringLiteral("stepSize"), 0.05},
            {QStringLiteral("neutralValue"), 0.0},
            {QStringLiteral("value"), 0.5},
            {QStringLiteral("width"), 320.0},
        }
    );
    auto next_keyboard_slider = createSourceComponent(
        engine,
        QStringLiteral("ShadowSlider.qml"),
        {
            {QStringLiteral("label"), QStringLiteral("Contrast")},
            {QStringLiteral("from"), 0.25},
            {QStringLiteral("to"), 2.5},
            {QStringLiteral("stepSize"), 0.01},
            {QStringLiteral("neutralValue"), 1.0},
            {QStringLiteral("value"), 1.0},
            {QStringLiteral("width"), 320.0},
        }
    );
    if (!keyboard_slider || !next_keyboard_slider) {
        return EXIT_FAILURE;
    }
    QObject::connect(
        keyboard_slider.get(),
        SIGNAL(edited(double)),
        &recorder,
        SLOT(recordEdited(double))
    );
    QObject::connect(
        keyboard_slider.get(),
        SIGNAL(gestureStarted()),
        &recorder,
        SLOT(recordGestureStarted())
    );
    QObject::connect(
        keyboard_slider.get(),
        SIGNAL(gestureFinished()),
        &recorder,
        SLOT(recordGestureFinished())
    );

    QQuickWindow keyboard_window;
    keyboard_window.resize(360, 96);
    auto* const keyboard_slider_item = qobject_cast<QQuickItem*>(keyboard_slider.get());
    auto* const next_keyboard_slider_item =
        qobject_cast<QQuickItem*>(next_keyboard_slider.get());
    keyboard_slider_item->setParentItem(keyboard_window.contentItem());
    keyboard_slider_item->setPosition(QPointF{20.0, 12.0});
    next_keyboard_slider_item->setParentItem(keyboard_window.contentItem());
    next_keyboard_slider_item->setPosition(QPointF{20.0, 50.0});

    QQmlComponent consuming_surface_component(&engine);
    consuming_surface_component.setData(
        R"QML(
            import QtQuick
            Rectangle {
                width: 16
                height: 24
                color: "transparent"
                property int tapCount: 0
                TapHandler { onTapped: parent.tapCount += 1 }
            }
        )QML",
        QUrl(QStringLiteral("inmemory:/ConsumingBlankSurface.qml"))
    );
    while (consuming_surface_component.isLoading()) {
        drainBindings();
    }
    std::unique_ptr<QObject> consuming_surface(consuming_surface_component.create());
    if (!consuming_surface) {
        std::cerr << consuming_surface_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto* const consuming_surface_item = qobject_cast<QQuickItem*>(consuming_surface.get());
    consuming_surface_item->setParentItem(keyboard_window.contentItem());
    consuming_surface_item->setPosition(QPointF{0.0, 72.0});
    consuming_surface_item->setZ(10.0);
    keyboard_window.show();
    drainBindings();

    auto* const value_label = keyboard_slider->findChild<QQuickItem*>(
        QStringLiteral("shadowSliderValueLabel")
    );
    QObject* const value_editor = keyboard_slider->findChild<QObject*>(
        QStringLiteral("shadowSliderValueEditor")
    );
    auto* const keyboard_input = keyboard_slider->findChild<QQuickItem*>(
        QStringLiteral("shadowAdjustmentSliderInput")
    );
    auto* const next_keyboard_input = next_keyboard_slider->findChild<QQuickItem*>(
        QStringLiteral("shadowAdjustmentSliderInput")
    );
    if (!require(value_label != nullptr, "the displayed value exposes its click target")
        || !require(value_editor != nullptr, "the slider owns a compact numeric editor")
        || !require(keyboard_input != nullptr && next_keyboard_input != nullptr,
                    "adjustment sliders expose their keyboard focus targets")) {
        return EXIT_FAILURE;
    }

    auto* const keyboard_handle = keyboard_slider->findChild<QQuickItem*>(
        QStringLiteral("shadowInlineSliderHandle")
    );
    if (!require(keyboard_handle != nullptr, "the adjustment slider exposes its real thumb")) {
        return EXIT_FAILURE;
    }
    const QPointF keyboard_handle_center = keyboard_handle->mapToItem(
        keyboard_window.contentItem(),
        QPointF{keyboard_handle->width() / 2.0, keyboard_handle->height() / 2.0}
    );
    const int edits_before_real_reset = recorder.edited_count;
    QTest::mouseDClick(
        &keyboard_window,
        Qt::LeftButton,
        Qt::NoModifier,
        keyboard_handle_center.toPoint()
    );
    drainBindings();
    if (!require(
            keyboard_slider->property("value").toDouble() == 0.0,
            "a real thumb double click synchronizes the visible slider to its default"
        )
        || !require(
            recorder.edited_count == edits_before_real_reset + 1
                && std::abs(recorder.edited_value) < 0.000'001,
            "a real thumb double click emits one matching default edit"
        )) {
        return EXIT_FAILURE;
    }

    const QPointF value_label_center = value_label->mapToItem(
        keyboard_window.contentItem(),
        QPointF{value_label->width() / 2.0, value_label->height() / 2.0}
    );
    QTest::mouseClick(
        &keyboard_window,
        Qt::LeftButton,
        Qt::NoModifier,
        value_label_center.toPoint()
    );
    drainBindings();
    if (!require(
            keyboard_slider->property("valueEditing").toBool(),
            "clicking the displayed value enters numeric edit mode"
        )
        || !require(
            value_editor->property("activeFocus").toBool(),
            "numeric edit mode focuses and selects the compact editor"
        )) {
        return EXIT_FAILURE;
    }

    const int edits_before_numeric_entry = recorder.edited_count;
    const int starts_before_numeric_entry = recorder.gesture_started_count;
    const int finishes_before_numeric_entry = recorder.gesture_finished_count;
    value_editor->setProperty("text", QStringLiteral("1.50"));
    QTest::keyClick(&keyboard_window, Qt::Key_Return);
    drainBindings();
    if (!require(
            recorder.edited_count == edits_before_numeric_entry + 1
                && std::abs(recorder.edited_value - 1.5) < 0.000'001,
            "manual numeric entry emits the converted bounded slider value exactly once"
        )
        || !require(
            std::abs(keyboard_slider->property("value").toDouble() - 1.5)
                < 0.000'001,
            "manual numeric entry synchronizes the visible slider before model feedback"
        )
        || !require(
            recorder.gesture_started_count == starts_before_numeric_entry + 1
                && recorder.gesture_finished_count == finishes_before_numeric_entry + 1,
            "manual numeric entry remains one undoable adjustment gesture"
        )
        || !require(
            !keyboard_slider->property("valueEditing").toBool(),
            "accepting numeric entry returns to slider interaction"
        )) {
        return EXIT_FAILURE;
    }

    const int edits_before_outside_click = recorder.edited_count;
    QTest::mouseClick(
        &keyboard_window,
        Qt::LeftButton,
        Qt::NoModifier,
        value_label_center.toPoint()
    );
    drainBindings();
    value_editor->setProperty("text", QStringLiteral("0.75"));
    QTest::mouseClick(
        &keyboard_window,
        Qt::LeftButton,
        Qt::NoModifier,
        QPoint{6, 90}
    );
    drainBindings();
    if (!require(
            !keyboard_slider->property("valueEditing").toBool(),
            "clicking blank window content closes the numeric editor"
        )
        || !require(
            recorder.edited_count == edits_before_outside_click + 1
                && std::abs(recorder.edited_value - 0.75) < 0.000'001,
            "clicking blank content commits the numeric value exactly once"
        )
        || !require(
            consuming_surface->property("tapCount").toInt() == 1,
            "outside-click dismissal remains passive and preserves the target click"
        )) {
        return EXIT_FAILURE;
    }

    keyboard_input->forceActiveFocus(Qt::TabFocusReason);
    drainBindings();
    const double value_before_arrow = keyboard_slider->property("value").toDouble();
    const int edits_before_arrow = recorder.edited_count;
    QTest::keyClick(&keyboard_window, Qt::Key_Right);
    drainBindings();
    if (!require(
            keyboard_slider->property("value").toDouble() > value_before_arrow,
            "right arrow performs native step-sized fine adjustment"
        )
        || !require(
            recorder.edited_count == edits_before_arrow + 1,
            "keyboard fine adjustment follows the public edited-value route"
        )) {
        return EXIT_FAILURE;
    }

    QTest::keyClick(&keyboard_window, Qt::Key_Down);
    drainBindings();
    if (!require(
            next_keyboard_input->hasActiveFocus(),
            "down arrow moves focus to the next adjustment slider"
        )) {
        return EXIT_FAILURE;
    }
    QTest::keyClick(&keyboard_window, Qt::Key_Up);
    drainBindings();
    if (!require(
            keyboard_input->hasActiveFocus(),
            "up arrow moves focus to the previous adjustment slider"
        )) {
        return EXIT_FAILURE;
    }

    // Precision's adjustment inspector hosts sliders inside a ScrollView.  A
    // blank click there is first observed by the Flickable viewport, so this
    // real hierarchy must retain the same numeric-editor dismissal contract
    // as an ordinary window child.
    QQmlComponent panel_component(&engine);
    panel_component.setData(
        R"QML(
            import QtQuick
            import QtQuick.Controls
            ScrollView {
                width: 340
                height: 150
                contentWidth: availableWidth
                contentHeight: 260

                Item {
                    objectName: "adjustmentPanelContent"
                    width: 340
                    height: 260

                    Rectangle {
                        objectName: "adjustmentPanelBlankTarget"
                        x: 0
                        y: 70
                        width: parent.width
                        height: 190
                        color: "transparent"
                        property int tapCount: 0
                        TapHandler { onTapped: parent.tapCount += 1 }
                    }
                }
            }
        )QML",
        QUrl(QStringLiteral("inmemory:/AdjustmentPanelSurface.qml"))
    );
    while (panel_component.isLoading()) {
        drainBindings();
    }
    std::unique_ptr<QObject> panel_surface(panel_component.create());
    if (!panel_surface) {
        std::cerr << panel_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto panel_slider = createSourceComponent(
        engine,
        QStringLiteral("ShadowSlider.qml"),
        {
            {QStringLiteral("label"), QStringLiteral("Temperature")},
            {QStringLiteral("from"), 2'000.0},
            {QStringLiteral("to"), 25'000.0},
            {QStringLiteral("neutralValue"), 5'500.0},
            {QStringLiteral("value"), 5'668.0},
            {QStringLiteral("width"), 300.0},
        }
    );
    if (!panel_slider) {
        return EXIT_FAILURE;
    }
    QObject::connect(
        panel_slider.get(),
        SIGNAL(edited(double)),
        &recorder,
        SLOT(recordEdited(double))
    );

    QQuickWindow panel_window;
    panel_window.resize(360, 180);
    auto* const panel_surface_item = qobject_cast<QQuickItem*>(panel_surface.get());
    auto* const panel_content = panel_surface->findChild<QQuickItem*>(
        QStringLiteral("adjustmentPanelContent")
    );
    QObject* const panel_blank_target = panel_surface->findChild<QObject*>(
        QStringLiteral("adjustmentPanelBlankTarget")
    );
    auto* const panel_slider_item = qobject_cast<QQuickItem*>(panel_slider.get());
    if (!require(panel_surface_item != nullptr && panel_content != nullptr
                     && panel_blank_target != nullptr,
                 "the adjustment-panel fixture exposes its ScrollView content")) {
        return EXIT_FAILURE;
    }
    panel_surface_item->setParentItem(panel_window.contentItem());
    panel_surface_item->setPosition(QPointF{10.0, 10.0});
    panel_slider_item->setParentItem(panel_content);
    panel_slider_item->setPosition(QPointF{20.0, 18.0});
    panel_window.show();
    drainBindings();

    auto* const panel_value_label = panel_slider->findChild<QQuickItem*>(
        QStringLiteral("shadowSliderValueLabel")
    );
    QObject* const panel_value_editor = panel_slider->findChild<QObject*>(
        QStringLiteral("shadowSliderValueEditor")
    );
    if (!require(panel_value_label != nullptr && panel_value_editor != nullptr,
                 "the adjustment-panel slider exposes its numeric editor")) {
        return EXIT_FAILURE;
    }
    const QPointF panel_value_label_center = panel_value_label->mapToItem(
        panel_window.contentItem(),
        QPointF{panel_value_label->width() / 2.0, panel_value_label->height() / 2.0}
    );
    QTest::mouseClick(
        &panel_window,
        Qt::LeftButton,
        Qt::NoModifier,
        panel_value_label_center.toPoint()
    );
    drainBindings();
    const int edits_before_panel_blank = recorder.edited_count;
    panel_value_editor->setProperty("text", QStringLiteral("6000"));
    QTest::mouseClick(
        &panel_window,
        Qt::LeftButton,
        Qt::NoModifier,
        QPoint{40, 118}
    );
    drainBindings();
    if (!require(
            !panel_slider->property("valueEditing").toBool(),
            "clicking blank ScrollView panel content closes the numeric editor"
        )
        || !require(
            recorder.edited_count == edits_before_panel_blank + 1
                && std::abs(recorder.edited_value - 6'000.0) < 0.000'001,
            "panel blank dismissal commits the numeric value exactly once"
        )
        || !require(
            panel_blank_target->property("tapCount").toInt() == 1,
            "panel blank dismissal preserves the clicked panel target"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "shadow_adjustment_controls_contract_test.moc"
