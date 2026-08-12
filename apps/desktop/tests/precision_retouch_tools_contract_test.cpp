#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FakeRetouchEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy CONSTANT)
    Q_PROPERTY(QVariantList retouchStrokes READ retouchStrokes NOTIFY parametersChanged)
    Q_PROPERTY(QVariantList retouchSpots READ retouchSpots NOTIFY parametersChanged)
    Q_PROPERTY(int retouchCreationMode READ retouchCreationMode NOTIFY retouchCreationModeChanged)
    Q_PROPERTY(bool retouchPickerActive READ retouchPickerActive NOTIFY retouchPickerActiveChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool stateBusy() const noexcept {
        return false;
    }
    [[nodiscard]] QVariantList retouchStrokes() const {
        return strokes_;
    }
    [[nodiscard]] QVariantList retouchSpots() const {
        return spots_;
    }
    [[nodiscard]] int retouchCreationMode() const noexcept {
        return creation_mode_;
    }
    [[nodiscard]] bool retouchPickerActive() const noexcept {
        return picker_active_;
    }

    void publish(QVariantList strokes, QVariantList spots) {
        strokes_ = std::move(strokes);
        spots_ = std::move(spots);
        emit parametersChanged();
    }

    Q_INVOKABLE void setRetouchCreationMode(const int mode) {
        creation_mode_ = mode;
        emit retouchCreationModeChanged();
    }
    Q_INVOKABLE void setRetouchPickerActive(const bool active) {
        picker_active_ = active;
        emit retouchPickerActiveChanged();
    }
    Q_INVOKABLE void beginParameterEdit(const QString& key) {
        begin_key_ = key;
    }
    Q_INVOKABLE void endParameterEdit(const QString& key) {
        end_key_ = key;
    }
    Q_INVOKABLE void setRetouchStrokeMode(int, int) {}
    Q_INVOKABLE void setRetouchSpotMode(int, int) {}
    Q_INVOKABLE void setRetouchStrokeRadius(int, int) {}
    Q_INVOKABLE void setRetouchSpotRadius(int, int) {}
    Q_INVOKABLE void setRetouchStrokeFeather(int, double) {}
    Q_INVOKABLE void setRetouchSpotFeather(int, double) {}
    Q_INVOKABLE void setRetouchStrokeStrength(int, double) {}
    Q_INVOKABLE void setRetouchSpotStrength(int, double) {}
    Q_INVOKABLE void removeRetouchStroke(int) {}
    Q_INVOKABLE void removeRetouchSpot(int) {}
    Q_INVOKABLE void
    setRetouchStrokeSourceOffset(const int index, const double offset_x, const double offset_y) {
        ++stroke_source_write_count_;
        stroke_source_index_ = index;
        stroke_source_offset_x_ = offset_x;
        stroke_source_offset_y_ = offset_y;
    }
    Q_INVOKABLE void setRetouchSpotCenter(int, double, double) {}
    Q_INVOKABLE void
    setRetouchSpotSourceOffset(const int index, const double offset_x, const double offset_y) {
        ++spot_source_write_count_;
        spot_source_index_ = index;
        spot_source_offset_x_ = offset_x;
        spot_source_offset_y_ = offset_y;
    }

    QString begin_key_;
    QString end_key_;
    int stroke_source_write_count_ = 0;
    int stroke_source_index_ = -1;
    double stroke_source_offset_x_ = 0.0;
    double stroke_source_offset_y_ = 0.0;
    int spot_source_write_count_ = 0;
    int spot_source_index_ = -1;
    double spot_source_offset_x_ = 0.0;
    double spot_source_offset_y_ = 0.0;

  signals:
    void parametersChanged();
    void retouchCreationModeChanged();
    void retouchPickerActiveChanged();

  private:
    QVariantList strokes_;
    QVariantList spots_;
    int creation_mode_ = 0;
    bool picker_active_ = true;
};

class FakeRetouchInspector final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* editor READ editor CONSTANT)

  public:
    explicit FakeRetouchInspector(QObject* editor) : editor_(editor) {}

    [[nodiscard]] QObject* editor() const noexcept {
        return editor_;
    }

  private:
    QObject* editor_;
};

class SelectionRecorder final : public QObject {
    Q_OBJECT

  public slots:
    void record(const bool continuous, const int index) {
        ++count;
        last_continuous = continuous;
        last_index = index;
    }
    void recordTarget() {
        ++target_count;
    }

  public:
    int count = 0;
    bool last_continuous = false;
    int last_index = -2;
    int target_count = 0;
};

namespace {

[[nodiscard]] QVariantMap region(const int index, const int mode, const int radius) {
    return {
        {QStringLiteral("index"), index},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("radius"), radius},
        {QStringLiteral("feather"), 0.28},
        {QStringLiteral("strength"), 0.62},
    };
}

[[nodiscard]] QVariantMap strokeRegion(const int index, const double source_offset_x = 0.0) {
    QVariantMap value = region(index, 0, 18);
    value.insert(QStringLiteral("sourceOffsetX"), source_offset_x);
    value.insert(QStringLiteral("sourceOffsetY"), 0.0);
    value.insert(
        QStringLiteral("points"),
        QVariantList{
            QVariantMap{
                {QStringLiteral("x"), 0.25},
                {QStringLiteral("y"), 0.5},
                {QStringLiteral("beginsStroke"), true},
            },
            QVariantMap{
                {QStringLiteral("x"), 0.75},
                {QStringLiteral("y"), 0.5},
                {QStringLiteral("beginsStroke"), false},
            },
        }
    );
    return value;
}

[[nodiscard]] QVariantMap spotRegion(const int index, const double source_offset_x = 0.0) {
    QVariantMap value = region(index, 0, 18);
    value.insert(QStringLiteral("x"), 0.4);
    value.insert(QStringLiteral("y"), 0.5);
    value.insert(QStringLiteral("sourceOffsetX"), source_offset_x);
    value.insert(QStringLiteral("sourceOffsetY"), 0.0);
    return value;
}

[[nodiscard]] bool
invokeContains(QObject* const target, const char* const method, const double x, const double y) {
    QVariant result;
    return QMetaObject::invokeMethod(
               target,
               method,
               Q_RETURN_ARG(QVariant, result),
               Q_ARG(QVariant, x),
               Q_ARG(QVariant, y)
           )
           && result.toBool();
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void sendClick(QQuickWindow& window, const QPointF& position) {
    QMouseEvent press(
        QEvent::MouseButtonPress,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &press);
    QMouseEvent release(
        QEvent::MouseButtonRelease,
        position,
        position,
        position,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &release);
    drainBindings();
}

void sendDrag(QQuickWindow& window, const QPointF& start, const QPointF& finish) {
    QMouseEvent press(
        QEvent::MouseButtonPress,
        start,
        start,
        start,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &press);
    QMouseEvent move(
        QEvent::MouseMove,
        finish,
        finish,
        finish,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &move);
    QMouseEvent release(
        QEvent::MouseButtonRelease,
        finish,
        finish,
        finish,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &release);
    drainBindings();
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision retouch-tools contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    FakeRetouchEditor editor;
    editor.publish({region(0, 0, 18), region(1, 1, 24)}, {region(0, 0, 12)});
    FakeRetouchInspector inspector(&editor);
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionRetouchTools.qml")
        )
    );
    std::unique_ptr<QObject> tools(component.createWithInitialProperties({
        {QStringLiteral("inspector"), QVariant::fromValue(&inspector)},
        {QStringLiteral("currentTabIndex"), 0},
        {QStringLiteral("selectedRegionContinuous"), true},
        {QStringLiteral("selectedRegionIndex"), 1},
        {QStringLiteral("width"), 190.0},
        {QStringLiteral("height"), 640.0},
    }));
    if (!tools) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    drainBindings();

    SelectionRecorder selection;
    QObject::connect(
        tools.get(),
        SIGNAL(regionSelectionRequested(bool, int)),
        &selection,
        SLOT(record(bool, int))
    );

    const auto inspectors =
        tools->findChildren<QObject*>(QStringLiteral("retouchSelectedRegionInspector"));
    QObject* const strength_slider =
        tools->findChild<QObject*>(QStringLiteral("retouchStrengthSlider"));
    if (!require(
            tools->property("regionCount").toInt() == 3,
            "the compact picker exposes every authored region"
        )
        || !require(
            tools->property("regionInspectorCount").toInt() == 1 && inspectors.size() == 1,
            "all regions share exactly one selected-region inspector"
        )
        || !require(
            tools->property("selectedRegionDisplayIndex").toInt() == 1,
            "the selected continuous region keeps its display position"
        )
        || !require(
            strength_slider != nullptr
                && strength_slider->property("neutralValue").toDouble() == 1.0
                && strength_slider->property("value").toDouble() == 0.62,
            "the selected repair exposes a persisted strength slider that resets to 100 percent"
        )
        || !require(
            QMetaObject::invokeMethod(
                tools.get(),
                "selectRegion",
                Q_ARG(QVariant, false),
                Q_ARG(QVariant, 0)
            ) && selection.count == 1
                && !selection.last_continuous && selection.last_index == 0,
            "a compact region button emits one cross-panel selection"
        )) {
        return EXIT_FAILURE;
    }

    tools->setProperty("selectedRegionContinuous", false);
    tools->setProperty("selectedRegionIndex", 0);
    drainBindings();
    if (!require(
            tools->property("selectedRegionDisplayIndex").toInt() == 2,
            "legacy spots follow continuous strokes in one stable region order"
        )) {
        return EXIT_FAILURE;
    }

    editor.publish(
        {
            region(0, 0, 18),
            region(1, 1, 24),
            region(2, 0, 20),
        },
        {region(0, 0, 12)}
    );
    drainBindings();
    if (!require(
            selection.count == 2 && selection.last_continuous && selection.last_index == 2,
            "a newly painted region becomes the single inspected region"
        )) {
        return EXIT_FAILURE;
    }

    tools->setProperty("selectedRegionContinuous", true);
    tools->setProperty("selectedRegionIndex", 2);
    editor.publish({}, {});
    drainBindings();
    if (!require(
            selection.count == 3 && selection.last_continuous && selection.last_index == -1,
            "removing the last region clears the shared selection"
        )) {
        return EXIT_FAILURE;
    }

    QVariantList many_regions;
    for (int index = 0; index < 12; ++index) {
        many_regions.append(region(index, index % 2, 18));
    }
    editor.publish(many_regions, {});
    tools->setProperty("selectedRegionContinuous", true);
    tools->setProperty("selectedRegionIndex", 11);
    drainBindings();
    QObject* const region_picker =
        tools->findChild<QObject*>(QStringLiteral("retouchRegionPicker"));
    QObject* const region_flickable =
        tools->findChild<QObject*>(QStringLiteral("retouchRegionFlickable"));
    if (region_picker != nullptr && region_flickable != nullptr) {
        region_flickable->setProperty("width", 120.0);
        QMetaObject::invokeMethod(region_picker, "revealSelected");
        drainBindings();
    }
    if (!require(
            region_picker != nullptr && region_flickable != nullptr
                && region_picker->property("focusRingGutter").toInt() == 2
                && region_flickable->property("contentWidth").toDouble()
                       > region_flickable->property("width").toDouble()
                && region_flickable->property("contentX").toDouble() > 0.0
                && region_flickable->property("contentX").toDouble()
                           + region_flickable->property("width").toDouble()
                       >= region_flickable->property("contentWidth").toDouble() - 0.5,
            "the current region is auto-revealed with room for its clipped focus ring"
        )
        || !require(
            region_picker->property("selectedDisplayIndex").toInt() == 11,
            "the selected region exposes one stable selected state"
        )) {
        return EXIT_FAILURE;
    }

    QQmlComponent stroke_component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionRetouchStrokeHandle.qml")
        )
    );
    std::unique_ptr<QObject> stroke_handle(stroke_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("modelData"), strokeRegion(0)},
        {QStringLiteral("pixelScale"), 1.0},
        {QStringLiteral("selected"), false},
        {QStringLiteral("width"), 200.0},
        {QStringLiteral("height"), 100.0},
    }));
    if (!stroke_handle) {
        std::cerr << stroke_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    drainBindings();
    QObject* const target_hit_area =
        stroke_handle->findChild<QObject*>(QStringLiteral("retouchStrokeTargetHitArea"));
    QObject* const source_hit_area =
        stroke_handle->findChild<QObject*>(QStringLiteral("retouchStrokeSourceHitArea"));
    QObject::connect(
        stroke_handle.get(),
        SIGNAL(selectedRequested()),
        &selection,
        SLOT(recordTarget())
    );
    QQuickWindow stroke_window;
    stroke_window.setGeometry(0, 0, 200, 100);
    auto* const stroke_item = qobject_cast<QQuickItem*>(stroke_handle.get());
    if (stroke_item != nullptr) {
        stroke_item->setParentItem(stroke_window.contentItem());
    }
    stroke_window.show();
    drainBindings();
    if (stroke_item != nullptr) {
        sendClick(stroke_window, QPointF(80.0, 50.0));
    }
    if (!require(
            target_hit_area != nullptr && source_hit_area != nullptr
                && target_hit_area->property("enabled").toBool()
                && !source_hit_area->property("enabled").toBool(),
            "an unselected stroke catches its target but never lets its donor steal input"
        )
        || !require(
            invokeContains(stroke_handle.get(), "targetContains", 80.0, 50.0)
                && !invokeContains(stroke_handle.get(), "targetContains", 80.0, 5.0)
                && invokeContains(stroke_handle.get(), "sourceContains", 80.0, 50.0),
            "target and donor hit tests follow swept coverage instead of a bounding box"
        )
        || !require(
            stroke_item != nullptr && selection.target_count == 1 && editor.begin_key_.isEmpty(),
            "target-stroke admission emits selection before the painter can receive the press"
        )) {
        return EXIT_FAILURE;
    }
    stroke_handle->setProperty("selected", true);
    editor.begin_key_.clear();
    drainBindings();
    if (!require(
            source_hit_area->property("enabled").toBool()
                && source_hit_area->property("z").toDouble()
                       > target_hit_area->property("z").toDouble(),
            "a selected overlapping donor keeps drag priority above its target"
        )) {
        return EXIT_FAILURE;
    }
    sendClick(stroke_window, QPointF(80.0, 50.0));
    if (!require(
            editor.begin_key_ == QStringLiteral("retouch/stroke/0/source"),
            "the selected donor keeps its drag gesture when it overlaps the target"
        )) {
        return EXIT_FAILURE;
    }

    QQmlComponent spot_component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionRetouchSpotHandle.qml")
        )
    );
    std::unique_ptr<QObject> spot_handle(spot_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("modelData"), spotRegion(0)},
        {QStringLiteral("pixelScale"), 1.0},
        {QStringLiteral("selected"), false},
        {QStringLiteral("width"), 200.0},
        {QStringLiteral("height"), 100.0},
    }));
    if (!spot_handle) {
        std::cerr << spot_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    drainBindings();
    QObject* const spot_target_hit_area =
        spot_handle->findChild<QObject*>(QStringLiteral("retouchSpotTargetHitArea"));
    QObject* const spot_source_hit_area =
        spot_handle->findChild<QObject*>(QStringLiteral("retouchSpotSourceHitArea"));
    QObject::connect(
        spot_handle.get(),
        SIGNAL(selectedRequested()),
        &selection,
        SLOT(recordTarget())
    );
    QQuickWindow spot_window;
    spot_window.setGeometry(0, 0, 200, 100);
    auto* const spot_item = qobject_cast<QQuickItem*>(spot_handle.get());
    if (spot_item != nullptr) {
        spot_item->setParentItem(spot_window.contentItem());
    }
    spot_window.show();
    drainBindings();
    editor.begin_key_.clear();
    const int spot_selection_count_before_click = selection.target_count;
    if (spot_item != nullptr) {
        sendClick(spot_window, QPointF(80.0, 50.0));
    }
    if (!require(
            spot_target_hit_area != nullptr && spot_source_hit_area != nullptr
                && spot_target_hit_area->property("enabled").toBool()
                && spot_source_hit_area->property("visible").toBool()
                && !spot_source_hit_area->property("enabled").toBool(),
            "an unselected spot keeps its donor visible without letting it intercept input"
        )
        || !require(
            spot_item != nullptr && selection.target_count == spot_selection_count_before_click + 1
                && editor.begin_key_ == QStringLiteral("retouch/0/center"),
            "an overlapping unselected donor leaves target selection and movement in control"
        )) {
        return EXIT_FAILURE;
    }

    spot_handle->setProperty("selected", true);
    editor.begin_key_.clear();
    drainBindings();
    if (!require(
            spot_source_hit_area->property("enabled").toBool(),
            "the selected spot donor becomes interactive"
        )) {
        return EXIT_FAILURE;
    }
    sendClick(spot_window, QPointF(80.0, 50.0));
    if (!require(
            editor.begin_key_ == QStringLiteral("retouch/0/source"),
            "the selected overlapping spot donor keeps drag priority above its target"
        )) {
        return EXIT_FAILURE;
    }

    stroke_handle->setProperty("pixelScale", 0.1);
    stroke_handle->setProperty("modelData", strokeRegion(0, 2.0));
    spot_handle->setProperty("pixelScale", 0.1);
    spot_handle->setProperty("modelData", spotRegion(0, 2.0));
    drainBindings();
    if (!require(
            std::abs(stroke_handle->property("radiusPixels").toDouble() - 1.8) < 0.01
                && std::abs(stroke_handle->property("sourceOffsetX").toDouble() - 3.6) < 0.01,
            "fit-view stroke coverage and donor displacement use exact geometry"
        )
        || !require(
            std::abs(spot_handle->property("radiusPixels").toDouble() - 1.8) < 0.01
                && std::abs(
                       spot_handle->property("sourceX").toDouble()
                       - spot_handle->property("targetX").toDouble() - 3.6
                   ) < 0.01,
            "fit-view spot coverage and donor displacement use exact geometry"
        )) {
        return EXIT_FAILURE;
    }

    editor.stroke_source_write_count_ = 0;
    editor.stroke_source_index_ = -1;
    editor.end_key_.clear();
    sendDrag(stroke_window, QPointF(84.0, 50.0), QPointF(92.0, 54.0));
    if (!require(
            editor.stroke_source_write_count_ > 0 && editor.stroke_source_index_ == 0
                && std::abs(editor.stroke_source_offset_x_ - 6.44) < 0.15
                && std::abs(editor.stroke_source_offset_y_ - 2.22) < 0.15
                && editor.end_key_ == QStringLiteral("retouch/stroke/0/source"),
            "a selected fit-view stroke donor follows a real drag without snapping"
        )) {
        return EXIT_FAILURE;
    }

    editor.spot_source_write_count_ = 0;
    editor.spot_source_index_ = -1;
    editor.end_key_.clear();
    sendDrag(spot_window, QPointF(84.0, 50.0), QPointF(92.0, 54.0));
    if (!require(
            editor.spot_source_write_count_ > 0 && editor.spot_source_index_ == 0
                && std::abs(editor.spot_source_offset_x_ - 6.44) < 0.15
                && std::abs(editor.spot_source_offset_y_ - 2.22) < 0.15
                && editor.end_key_ == QStringLiteral("retouch/0/source"),
            "a selected fit-view spot donor follows a real drag without snapping"
        )) {
        return EXIT_FAILURE;
    }

    editor.stroke_source_write_count_ = 0;
    sendDrag(stroke_window, QPointF(84.0, 50.0), QPointF(124.0, 50.0));
    if (!require(
            editor.stroke_source_write_count_ > 0 && editor.stroke_source_offset_x_ > 8.0,
            "a stroke donor can move beyond the former eight-radius cap"
        )) {
        return EXIT_FAILURE;
    }

    editor.spot_source_write_count_ = 0;
    sendDrag(spot_window, QPointF(84.0, 50.0), QPointF(124.0, 50.0));
    if (!require(
            editor.spot_source_write_count_ > 0 && editor.spot_source_offset_x_ > 8.0,
            "a spot donor can move beyond the former eight-radius cap"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_retouch_tools_contract_test.moc"
