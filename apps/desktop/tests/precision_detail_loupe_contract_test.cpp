#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class DetailEditorStub final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList detailTiles MEMBER detail_tiles NOTIFY detailChanged)
    Q_PROPERTY(bool detailRendering MEMBER detail_rendering NOTIFY detailChanged)
    Q_PROPERTY(bool fullResolutionPreparing MEMBER full_resolution_preparing NOTIFY detailChanged)
    Q_PROPERTY(QString detailErrorText MEMBER detail_error_text NOTIFY detailChanged)
    Q_PROPERTY(quint32 detailFullWidth MEMBER detail_full_width NOTIFY detailChanged)
    Q_PROPERTY(quint32 detailFullHeight MEMBER detail_full_height NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap photoGeometry MEMBER photo_geometry NOTIFY parametersChanged)
    Q_PROPERTY(bool liquifyNodeMaterialized MEMBER liquify_materialized NOTIFY parametersChanged)
    Q_PROPERTY(bool liquifyNodeEnabled MEMBER liquify_enabled NOTIFY parametersChanged)
    Q_PROPERTY(QVariantList liquifyStrokes MEMBER liquify_strokes NOTIFY parametersChanged)

  public:
    QVariantList detail_tiles;
    bool detail_rendering = false;
    bool full_resolution_preparing = false;
    QString detail_error_text;
    quint32 detail_full_width = 4'000;
    quint32 detail_full_height = 3'000;
    QVariantMap photo_geometry;
    bool liquify_materialized = false;
    bool liquify_enabled = false;
    QVariantList liquify_strokes;
    int detail_request_count = 0;
    double requested_center_x = 0.0;
    double requested_center_y = 0.0;
    int requested_width = 0;
    int requested_height = 0;

    Q_INVOKABLE void requestDetailViewport(
        const double center_x,
        const double center_y,
        const int width,
        const int height
    ) {
        ++detail_request_count;
        requested_center_x = center_x;
        requested_center_y = center_y;
        requested_width = width;
        requested_height = height;
    }

    Q_INVOKABLE void leaveDetailMode() {}

  signals:
    void detailChanged();
    void parametersChanged();
    void sourcePathChanged();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision detail loupe contract failed: " << message << '\n';
    }
    return condition;
}

void drain_bindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void send_mouse(
    QQuickWindow& window,
    const QEvent::Type type,
    const QPointF& position,
    const Qt::MouseButton button,
    const Qt::MouseButtons buttons
) {
    QMouseEvent event(
        type,
        position,
        position,
        position,
        button,
        buttons,
        Qt::NoModifier,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &event);
    drain_bindings();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.DetailLoupeContract"),
        QStringLiteral("PrecisionDetailLoupe")
    );

    DetailEditorStub editor;
    editor.detail_tiles = {QVariantMap{
        {QStringLiteral("x"), 1'792},
        {QStringLiteral("y"), 1'024},
        {QStringLiteral("width"), 1'024},
        {QStringLiteral("height"), 1'024},
        {QStringLiteral("source"), QString{}},
    }};
    std::unique_ptr<QObject> object{component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("followPointer"), false},
        {QStringLiteral("zoomFactor"), 1.0},
        {QStringLiteral("targetCenterX"), 0.5},
        {QStringLiteral("targetCenterY"), 0.5},
        {QStringLiteral("deviceScale"), 2.0},
        {QStringLiteral("dragTopBoundary"), 50.0},
        {QStringLiteral("targetKind"), QStringLiteral("camera")},
        {QStringLiteral("focusConfirmed"), true},
    })};
    auto* const root = qobject_cast<QQuickItem*>(object.get());
    if (!root) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    drain_bindings();

    bool valid = true;
    valid &= require(
        root->property("targetLabel").toString() == QStringLiteral("Confirmed camera focus"),
        "confirmed camera provenance"
    );
    valid &= require(
        root->property("detailViewportWidth").toDouble() > 0.0
            && root->property("detailViewportHeight").toDouble() > 0.0,
        "bounded detail viewport"
    );
    auto* const follow_button =
        root->findChild<QQuickItem*>(QStringLiteral("detailLoupeFollowButton"));
    auto* const busy_indicator =
        root->findChild<QQuickItem*>(QStringLiteral("detailLoupeBusyIndicator"));
    auto* const wait_text = root->findChild<QQuickItem*>(QStringLiteral("detailLoupeWaitText"));
    valid &= require(follow_button != nullptr, "pin/follow affordance");
    valid &= require(
        busy_indicator != nullptr && busy_indicator->property("running").toBool(),
        "queued detail remains visibly busy before the renderer starts"
    );
    valid &= require(
        wait_text != nullptr && !wait_text->isVisible(),
        "fast detail requests do not flash explanatory copy"
    );
    root->setProperty("waitMessageVisible", true);
    drain_bindings();
    valid &= require(
        wait_text != nullptr && wait_text->isVisible()
            && wait_text->property("text").toString() == QStringLiteral("Waiting for 100% detail…"),
        "a delayed queued detail request explains what it is waiting for"
    );
    editor.full_resolution_preparing = true;
    emit editor.detailChanged();
    drain_bindings();
    valid &= require(
        wait_text != nullptr
            && wait_text->property("text").toString() == QStringLiteral("Preparing 100% detail…"),
        "full-resolution preparation is distinguished from rendering"
    );
    valid &= require(
        follow_button != nullptr
            && follow_button->property("source").toUrl().toString()
                   == QStringLiteral("qrc:/icons/pin.svg"),
        "pin/follow affordance uses a pushpin rather than a map locator"
    );
    valid &= require(
        root->findChild<QQuickItem*>(QStringLiteral("detailLoupeDragRegion")) != nullptr,
        "title drag affordance"
    );
    valid &= require(
        root->findChild<QQuickItem*>(QStringLiteral("detailLoupe100Button")) != nullptr
            && root->findChild<QQuickItem*>(QStringLiteral("detailLoupe200Button")) != nullptr,
        "pixel zoom affordances"
    );

    const double initial_scale = root->property("detailImageScale").toDouble();
    const double initial_width = root->property("detailImageDisplayWidth").toDouble();
    const double initial_height = root->property("detailImageDisplayHeight").toDouble();
    const double source_center_x = (root->property("detailViewportWidth").toDouble() / 2.0
                                    - root->property("detailImageX").toDouble())
                                       / initial_scale
                                   + 1'792.0;
    const double source_center_y = (root->property("detailViewportHeight").toDouble() / 2.0
                                    - root->property("detailImageY").toDouble())
                                       / initial_scale
                                   + 1'024.0;
    valid &= require(
        std::abs(source_center_x - 2'000.0) < 1.0e-9
            && std::abs(source_center_y - 1'500.0) < 1.0e-9,
        "tile-aligned transport is cropped around the exact two-axis target"
    );
    root->setProperty("zoomFactor", 2.0);
    drain_bindings();
    valid &= require(
        std::abs(initial_scale - 0.5) < 1.0e-12
            && std::abs(root->property("detailImageScale").toDouble() - 1.0) < 1.0e-12,
        "pixel zoom accounts for device scale"
    );
    valid &= require(
        std::abs(root->property("detailImageDisplayWidth").toDouble() - initial_width * 2.0)
                < 1.0e-12
            && std::abs(
                   root->property("detailImageDisplayHeight").toDouble() - initial_height * 2.0
               ) < 1.0e-12,
        "200 percent scales both image axes equally"
    );

    QQuickWindow canvas_window;
    canvas_window.setGeometry(0, 0, 800, 600);
    root->setParentItem(canvas_window.contentItem());
    canvas_window.show();
    drain_bindings();
    QMetaObject::invokeMethod(root, "resetWindowPosition");
    drain_bindings();
    const QString capture_path = qEnvironmentVariable("SHADOW_DETAIL_LOUPE_CAPTURE_PATH");
    if (!capture_path.isEmpty()) {
        const QImage capture = canvas_window.grabWindow();
        valid &=
            require(!capture.isNull() && capture.save(capture_path), "optional visual capture");
    }
    valid &= require(
        std::abs(root->x() - root->property("maximumWindowX").toDouble()) < 1.0e-12
            && root->y() >= 50.0
            && root->x() + root->width() <= canvas_window.contentItem()->width() - 8.0 + 1.0e-12
            && root->y() + root->height() <= canvas_window.contentItem()->height() - 8.0 + 1.0e-12,
        "initial loupe position stays inside the central canvas"
    );

    const double initial_window_x = root->x();
    const double initial_window_y = root->y();
    const QPointF drag_start{initial_window_x + 40.0, initial_window_y + 18.0};
    const QPointF drag_admission{drag_start.x() - 20.0, drag_start.y() + 16.0};
    const QPointF drag_end{drag_start.x() - 96.0, drag_start.y() + 72.0};
    send_mouse(canvas_window, QEvent::MouseButtonPress, drag_start, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas_window, QEvent::MouseMove, drag_admission, Qt::NoButton, Qt::LeftButton);
    send_mouse(canvas_window, QEvent::MouseMove, drag_end, Qt::NoButton, Qt::LeftButton);
    send_mouse(canvas_window, QEvent::MouseButtonRelease, drag_end, Qt::LeftButton, Qt::NoButton);
    if (!(root->x() < initial_window_x - 40.0 && root->y() > initial_window_y + 30.0)) {
        auto* const drag_region =
            root->findChild<QQuickItem*>(QStringLiteral("detailLoupeDragRegion"));
        auto* const drag_handle =
            root->findChild<QQuickItem*>(QStringLiteral("detailLoupeDragHandle"));
        std::cerr << "Drag diagnostics: root=" << root->x() << ',' << root->y()
                  << " initial=" << initial_window_x << ',' << initial_window_y;
        if (drag_region != nullptr) {
            std::cerr << " region=" << drag_region->x() << ',' << drag_region->y() << ' '
                      << drag_region->width() << 'x' << drag_region->height();
        }
        if (drag_handle != nullptr) {
            std::cerr << " handle=" << drag_handle->x() << ',' << drag_handle->y() << ' '
                      << drag_handle->width() << 'x' << drag_handle->height()
                      << " enabled=" << drag_handle->isEnabled()
                      << " visible=" << drag_handle->isVisible();
        }
        std::cerr << '\n';
    }
    valid &= require(
        root->x() < initial_window_x - 40.0 && root->y() > initial_window_y + 30.0,
        "dragging the title moves the loupe in both canvas axes"
    );

    root->setX(-100.0);
    root->setY(1'000.0);
    QMetaObject::invokeMethod(root, "keepInsideCanvas");
    drain_bindings();
    valid &= require(
        root->x() >= 8.0 && root->y() >= 50.0
            && root->x() + root->width() <= canvas_window.contentItem()->width() - 8.0 + 1.0e-12
            && root->y() + root->height() <= canvas_window.contentItem()->height() - 8.0 + 1.0e-12,
        "drag bounds clamp the loupe to the central canvas"
    );

    root->setProperty("targetKind", QStringLiteral("manual"));
    root->setProperty("focusConfirmed", false);
    drain_bindings();
    valid &= require(
        root->property("targetLabel").toString() == QStringLiteral("Manual position"),
        "manual provenance"
    );

    editor.photo_geometry = {
        {QStringLiteral("present"), true},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("cropLeft"), 0.2},
        {QStringLiteral("cropTop"), 0.1},
        {QStringLiteral("cropRight"), 0.8},
        {QStringLiteral("cropBottom"), 0.9},
        {QStringLiteral("quarterTurn"), 1},
        {QStringLiteral("straightenDegrees"), 0.0},
        {QStringLiteral("flipHorizontal"), false},
        {QStringLiteral("flipVertical"), false},
    };
    const QVariantMap capture_metadata{
        {QStringLiteral("hasFocusObservation"), true},
        {QStringLiteral("focusObservationSchemaVersion"), 1},
        {QStringLiteral("focusObservationSource"), QStringLiteral("camera_focus_area")},
        {QStringLiteral("focusObservationCenterX"), 0.35},
        {QStringLiteral("focusObservationCenterY"), 0.30},
        {QStringLiteral("focusObservationConfirmed"), true},
    };
    QQmlComponent state_component{&engine};
    state_component.loadFromModule(
        QStringLiteral("Shadow.DetailLoupeContract"),
        QStringLiteral("PrecisionDetailLoupeState")
    );
    std::unique_ptr<QObject> state{state_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("captureMetadata"), capture_metadata},
        {QStringLiteral("requestAvailable"), true},
        {QStringLiteral("deviceScale"), 2.0},
        {QStringLiteral("viewportWidth"), 200.0},
        {QStringLiteral("viewportHeight"), 100.0},
    })};
    if (!state) {
        std::cerr << state_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QMetaObject::invokeMethod(state.get(), "open");
    drain_bindings();
    valid &= require(
        std::abs(state->property("centerX").toDouble() - 0.75) < 1.0e-12
            && std::abs(state->property("centerY").toDouble() - 0.25) < 1.0e-12,
        "crop and quarter-turn focus mapping"
    );
    valid &= require(
        state->property("targetKind").toString() == QStringLiteral("camera")
            && state->property("focusConfirmed").toBool(),
        "camera provenance survives mapping"
    );
    valid &= require(
        editor.detail_request_count == 1 && editor.requested_width == 400
            && editor.requested_height == 200,
        "device-pixel bounded detail request"
    );
    QMetaObject::invokeMethod(
        state.get(),
        "setZoomFactor",
        Q_ARG(QVariant, QVariant::fromValue(2.0))
    );
    drain_bindings();
    valid &= require(
        editor.detail_request_count == 2 && editor.requested_width == 200
            && editor.requested_height == 100,
        "200 percent halves both requested source dimensions"
    );

    editor.photo_geometry.insert(QStringLiteral("straightenDegrees"), 2.0);
    QMetaObject::invokeMethod(state.get(), "resetTargetFromMetadata");
    valid &= require(
        std::abs(state->property("centerX").toDouble() - 0.5) < 1.0e-12
            && std::abs(state->property("centerY").toDouble() - 0.5) < 1.0e-12
            && state->property("targetKind").toString() == QStringLiteral("center"),
        "non-affine geometry uses truthful center fallback"
    );

    root->setParentItem(nullptr);
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "precision_detail_loupe_contract_test.moc"
