#include "edit_stroke_input.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJSValue>
#include <QMouseEvent>
#include <QObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FakeDirectStrokeEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy NOTIFY stateBusyChanged)
    Q_PROPERTY(QVariantMap photoGeometry READ photoGeometry NOTIFY photoGeometryChanged)
    Q_PROPERTY(bool retouchPickerActive READ retouchPickerActive CONSTANT)
    Q_PROPERTY(int retouchBrushRadius READ retouchBrushRadius NOTIFY retouchBrushChanged)
    Q_PROPERTY(bool retouchSourceSampled READ retouchSourceSampled NOTIFY retouchSourceChanged)
    Q_PROPERTY(bool retouchSourcePicking READ retouchSourcePicking NOTIFY retouchSourceChanged)
    Q_PROPERTY(
        QVariantMap retouchSampledSource READ retouchSampledSource NOTIFY retouchSourceChanged
    )
    Q_PROPERTY(bool pointColorPickerActive READ pointColorPickerActive CONSTANT)
    Q_PROPERTY(bool whiteBalancePickerActive READ whiteBalancePickerActive CONSTANT)
    Q_PROPERTY(bool hasSelectedGradeNode READ hasSelectedGradeNode CONSTANT)
    Q_PROPERTY(QVariantMap selectedLocalMask READ selectedLocalMask CONSTANT)
    Q_PROPERTY(double liquifyBrushRadius READ liquifyBrushRadius CONSTANT)
    Q_PROPERTY(double liquifyBrushStrength READ liquifyBrushStrength CONSTANT)
    Q_PROPERTY(double liquifyBrushHardness READ liquifyBrushHardness CONSTANT)
    Q_PROPERTY(
        int liquifyBrushMode READ liquifyBrushMode WRITE setLiquifyBrushMode NOTIFY
            liquifyBrushChanged
    )
    Q_PROPERTY(bool liquifyCanReconstruct READ liquifyCanReconstruct NOTIFY parametersChanged)
    Q_PROPERTY(bool liquifyNodeEnabled READ liquifyNodeEnabled NOTIFY parametersChanged)
    Q_PROPERTY(QVariantList liquifyStrokes READ liquifyStrokes NOTIFY parametersChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool stateBusy() const noexcept {
        return state_busy;
    }
    void setStateBusy(const bool busy) {
        if (state_busy == busy) {
            return;
        }
        state_busy = busy;
        emit stateBusyChanged();
    }
    [[nodiscard]] QVariantMap photoGeometry() const {
        return {
            {QStringLiteral("quarterTurn"), 0},
            {QStringLiteral("flipHorizontal"), false},
            {QStringLiteral("flipVertical"), false},
            {QStringLiteral("cropLeft"), 0.1},
            {QStringLiteral("cropTop"), 0.1},
            {QStringLiteral("cropRight"), 0.9},
            {QStringLiteral("cropBottom"), 0.9},
        };
    }
    [[nodiscard]] bool retouchPickerActive() const noexcept {
        return true;
    }
    [[nodiscard]] int retouchBrushRadius() const noexcept {
        return retouch_brush_radius;
    }
    [[nodiscard]] bool retouchSourceSampled() const noexcept {
        return !retouch_sampled_source.isEmpty();
    }
    [[nodiscard]] bool retouchSourcePicking() const noexcept {
        return retouch_source_picking;
    }
    [[nodiscard]] QVariantMap retouchSampledSource() const {
        return retouch_sampled_source;
    }
    [[nodiscard]] bool pointColorPickerActive() const noexcept {
        return false;
    }
    [[nodiscard]] bool whiteBalancePickerActive() const noexcept {
        return false;
    }
    [[nodiscard]] bool hasSelectedGradeNode() const noexcept {
        return true;
    }
    [[nodiscard]] QVariantMap selectedLocalMask() const {
        return {
            {QStringLiteral("kind"), 3},
            {QStringLiteral("radiusX"), 0.035},
            {QStringLiteral("feather"), 0.6},
            {QStringLiteral("brushPoints"), QVariantList{}},
        };
    }
    [[nodiscard]] double liquifyBrushRadius() const noexcept {
        return 0.08;
    }
    [[nodiscard]] double liquifyBrushStrength() const noexcept {
        return 0.65;
    }
    [[nodiscard]] double liquifyBrushHardness() const noexcept {
        return 0.4;
    }
    [[nodiscard]] int liquifyBrushMode() const noexcept {
        return liquify_brush_mode;
    }
    void setLiquifyBrushMode(const int mode) {
        if (liquify_brush_mode == mode) {
            return;
        }
        liquify_brush_mode = mode;
        emit liquifyBrushChanged();
    }
    [[nodiscard]] bool liquifyCanReconstruct() const noexcept {
        return !liquify_strokes.isEmpty();
    }
    [[nodiscard]] bool liquifyNodeEnabled() const noexcept {
        return !liquify_strokes.isEmpty();
    }
    [[nodiscard]] QVariantList liquifyStrokes() const {
        return liquify_strokes;
    }

    Q_INVOKABLE void beginParameterEdit(const QString&) {}
    Q_INVOKABLE void endParameterEdit(const QString&) {}
    Q_INVOKABLE void setPhotoCropBounds(double, double, double, double) {}
    Q_INVOKABLE void setRetouchSourceFromPreview(const double x, const double y) {
        ++sample_source_count;
        retouch_source_picking = false;
        retouch_sampled_source = {
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), y},
        };
        emit retouchSourceChanged();
    }
    Q_INVOKABLE void setRetouchSourcePicking(const bool picking) {
        retouch_source_picking = picking;
        emit retouchSourceChanged();
    }
    Q_INVOKABLE void moveRetouchSourceFromPreview(const double x, const double y) {
        ++move_source_count;
        retouch_sampled_source = {
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), y},
        };
        emit retouchSourceChanged();
    }
    Q_INVOKABLE void adjustRetouchBrushRadius(const int direction) {
        retouch_brush_radius = std::clamp(retouch_brush_radius + direction, 1, 128);
        emit retouchBrushChanged();
    }
    Q_INVOKABLE void addRetouchSpotFromPreview(
        double,
        double,
        const QString& generation,
        const int level_zero_width,
        const int level_zero_height
    ) {
        ++spot_commit_count;
        committed_retouch_generation = generation;
        committed_retouch_width = level_zero_width;
        committed_retouch_height = level_zero_height;
    }
    Q_INVOKABLE void addRetouchStrokeFromPreview(
        const QVariantList& points,
        const QString& generation,
        const int level_zero_width,
        const int level_zero_height
    ) {
        ++stroke_commit_count;
        committed_points = points;
        committed_retouch_generation = generation;
        committed_retouch_width = level_zero_width;
        committed_retouch_height = level_zero_height;
    }
    Q_INVOKABLE void appendSelectedLocalMaskBrushStroke(const QVariantList& points) {
        ++mask_commit_count;
        committed_mask_points = points;
    }
    Q_INVOKABLE void
    addLiquifyStrokeFromPreview(const QVariantList& points, const double output_aspect_ratio) {
        ++liquify_commit_count;
        committed_liquify_points = points;
        committed_liquify_aspect = output_aspect_ratio;
        liquify_strokes.push_back(QVariantMap{{QStringLiteral("points"), points}});
        emit parametersChanged();
    }
    Q_INVOKABLE bool beginLiquifyLiveStroke() {
        ++liquify_live_begin_count;
        liquify_live_kind = liquify_brush_mode;
        return liquify_live_kind == 0 || liquifyCanReconstruct();
    }
    Q_INVOKABLE void updateLiquifyLiveStrokeFromPreview(
        const QVariantList& points,
        const double output_aspect_ratio
    ) {
        ++liquify_live_update_count;
        committed_liquify_points = points;
        committed_liquify_aspect = output_aspect_ratio;
    }
    Q_INVOKABLE void finishLiquifyLiveStroke() {
        ++liquify_live_finish_count;
        liquify_strokes.push_back(
            QVariantMap{
                {QStringLiteral("kind"), liquify_live_kind},
                {QStringLiteral("points"), committed_liquify_points},
            }
        );
        emit parametersChanged();
    }
    Q_INVOKABLE void cancelLiquifyLiveStroke() {
        ++liquify_live_cancel_count;
    }
    Q_INVOKABLE void setSelectedLocalMaskPoint(const QString&, double, double) {}
    Q_INVOKABLE void setSelectedLocalMaskValue(const QString&, double) {}
    Q_INVOKABLE void setWhiteBalanceFromPreview(double, double, const QString&) {}
    Q_INVOKABLE void addPointColorFromPreview(double, double, const QString&) {}

    int stroke_commit_count = 0;
    int spot_commit_count = 0;
    int sample_source_count = 0;
    int move_source_count = 0;
    int mask_commit_count = 0;
    int liquify_commit_count = 0;
    int liquify_live_begin_count = 0;
    int liquify_live_update_count = 0;
    int liquify_live_finish_count = 0;
    int liquify_live_cancel_count = 0;
    int liquify_live_kind = -1;
    QVariantList committed_points;
    QVariantList committed_mask_points;
    QVariantList committed_liquify_points;
    QVariantList liquify_strokes;
    QString committed_retouch_generation;
    double committed_liquify_aspect = 0.0;
    int committed_retouch_width = 0;
    int committed_retouch_height = 0;
    int liquify_brush_mode = 0;
    int retouch_brush_radius = 18;
    bool state_busy = false;
    bool retouch_source_picking = false;
    QVariantMap retouch_sampled_source;

  signals:
    void photoGeometryChanged();
    void parametersChanged();
    void selectedGradeNodeChanged();
    void stateBusyChanged();
    void retouchSourceChanged();
    void retouchBrushChanged();
    void liquifyBrushChanged();
};

class FakeLiquifyPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        bool transientLiquifyPending READ transientLiquifyPending NOTIFY transientLiquifyChanged
    )

  public:
    using QObject::QObject;

    [[nodiscard]] bool transientLiquifyPending() const noexcept {
        return transient_liquify_pending;
    }

    Q_INVOKABLE bool
    beginTransientLiquify(const double radius, const double strength, const double hardness) {
        ++begin_count;
        begin_radius = radius;
        begin_strength = strength;
        begin_hardness = hardness;
        return begin_allowed;
    }

    Q_INVOKABLE bool
    appendTransientLiquifyPoint(const double x, const double y, const double pressure) {
        ++append_count;
        last_point = {x, y};
        last_pressure = pressure;
        return true;
    }

    Q_INVOKABLE void finishTransientLiquify(const bool committed) {
        ++finish_count;
        finish_committed = committed;
        if (transient_liquify_pending != committed) {
            transient_liquify_pending = committed;
            emit transientLiquifyChanged();
        }
    }

    Q_INVOKABLE void cancelTransientLiquify() {
        ++cancel_count;
        if (transient_liquify_pending) {
            transient_liquify_pending = false;
            emit transientLiquifyChanged();
        }
    }

    void publishAuthoritativeFrame() {
        if (!transient_liquify_pending) {
            return;
        }
        transient_liquify_pending = false;
        emit transientLiquifyChanged();
    }

    int begin_count = 0;
    int append_count = 0;
    int finish_count = 0;
    int cancel_count = 0;
    double begin_radius = 0.0;
    double begin_strength = 0.0;
    double begin_hardness = 0.0;
    double last_pressure = 0.0;
    QPointF last_point;
    bool finish_committed = false;
    bool transient_liquify_pending = false;
    bool begin_allowed = true;

  signals:
    void transientLiquifyChanged();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision direct-stroke contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void sendMouse(
    QQuickWindow& window,
    const QEvent::Type type,
    const QPointF& position,
    const Qt::MouseButton button,
    const Qt::MouseButtons buttons,
    const Qt::KeyboardModifiers modifiers = Qt::NoModifier
) {
    QMouseEvent event(
        type,
        position,
        position,
        position,
        button,
        buttons,
        modifiers,
        QPointingDevice::primaryPointingDevice()
    );
    QGuiApplication::sendEvent(&window, &event);
    drainBindings();
}

[[nodiscard]] bool decoderContract() {
    const QVariantList valid{
        QVariantMap{
            {QStringLiteral("x"), 0.1},
            {QStringLiteral("y"), 0.2},
            {QStringLiteral("pressure"), 0.25},
        },
        QVariantMap{
            {QStringLiteral("x"), 0.1},
            {QStringLiteral("y"), 0.2},
            {QStringLiteral("pressure"), 0.75},
        },
        QVariantMap{
            {QStringLiteral("x"), 0.8},
            {QStringLiteral("y"), 0.7},
            {QStringLiteral("pressure"), 0.5},
        },
    };
    const auto decoded = EditStrokeInput::decodeNormalizedPoints(valid, 3);
    const auto samples = EditStrokeInput::decodeNormalizedSamples(valid, 3);
    return require(
               decoded.has_value() && decoded->size() == 2,
               "normalized transport validates and collapses adjacent duplicates"
           )
           && require(
               samples.has_value() && samples->size() == 2 && samples->front().pressure == 0.75
                   && samples->back().pressure == 0.5,
               "pressure is bounded and the latest colocated sample wins"
           )
           && require(
               !EditStrokeInput::decodeNormalizedPoints(
                    {QVariantMap{{QStringLiteral("x"), -0.1}, {QStringLiteral("y"), 0.2}}},
                    1
               )
                    .has_value(),
               "out-of-range points are rejected"
           )
           && require(
               !EditStrokeInput::decodeNormalizedPoints(valid, 2).has_value(),
               "the controller-side point limit is authoritative"
           )
           && require(
               !EditStrokeInput::decodeNormalizedSamples(
                    {
                        QVariantMap{
                            {QStringLiteral("x"), 0.2},
                            {QStringLiteral("y"), 0.3},
                            {QStringLiteral("pressure"), 1.1},
                        },
                    },
                    1
               )
                    .has_value(),
               "out-of-range pressure is rejected"
           );
}

[[nodiscard]] bool cloneSourceAnchorContract() {
    // A reduced copy of the looped cable-repair stroke that exposed the bug:
    // its first pointer sample lies on the left edge, while rendering and the
    // donor overlay transform around the stroke bounds midpoint.
    const QVector<QPointF> points{
        {0.5825106534090909, 0.29345703125},
        {0.5910822088068182, 0.29194557883522726},
        {0.6127041903409091, 0.2961270419034091},
        {0.6044122869318181, 0.29978249289772724},
        {0.5840553977272728, 0.29978249289772724},
    };
    const auto center = EditStrokeInput::normalizedBoundsCenter(
        std::span<const QPointF>(points.constData(), static_cast<std::size_t>(points.size()))
    );
    const QPointF sampled_source{0.497607421875, 0.29353777521306815};
    constexpr double width = 1'536.0;
    constexpr double height = 1'024.0;
    constexpr double radius = 18.0;
    if (!require(
            center.has_value() && std::abs(center->x() - 0.597607421875) < 1.0e-12
                && std::abs(center->y() - 0.29586403586647725) < 1.0e-12
                && std::abs(center->x() - points.constFirst().x()) > 0.01,
            "a looped repair uses the rendered coverage midpoint rather than its first edge sample"
        )) {
        return false;
    }
    const QPointF offset_radii{
        (sampled_source.x() - center->x()) * width / radius,
        (sampled_source.y() - center->y()) * height / radius,
    };
    return require(
        std::abs(center->x() + offset_radii.x() * radius / width - sampled_source.x()) < 1.0e-12
            && std::abs(center->y() + offset_radii.y() * radius / height - sampled_source.y())
                   < 1.0e-12,
        "the visible sampled-source crosshair is the actual donor coverage anchor"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    if (!decoderContract() || !cloneSourceAnchorContract()) {
        return EXIT_FAILURE;
    }

    QQmlEngine engine;
    FakeDirectStrokeEditor editor;

    QQmlComponent zoom_component(&engine);
    zoom_component.loadFromModule(
        QStringLiteral("Shadow.DirectStrokeContract"),
        QStringLiteral("PrecisionCanvasZoomInput")
    );
    std::unique_ptr<QObject> zoom(zoom_component.createWithInitialProperties({
        {QStringLiteral("interactionEnabled"), false},
        {QStringLiteral("toolActive"), false},
        {QStringLiteral("fitView"), true},
        {QStringLiteral("zoomFactor"), 1.0},
        {QStringLiteral("fitZoomFactor"), 1.0},
        {QStringLiteral("width"), 400.0},
        {QStringLiteral("height"), 300.0},
    }));
    if (!zoom) {
        std::cerr << zoom_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QObject* const zoom_cursor =
        zoom->findChild<QObject*>(QStringLiteral("canvasZoomMagnifierInput"));
    if (!require(
            !zoom->property("visible").toBool() && zoom_cursor != nullptr
                && zoom_cursor->property("cursorShape").toInt() == Qt::ArrowCursor,
            "an inactive zoom layer releases both hit testing and BlankCursor ownership"
        )) {
        return EXIT_FAILURE;
    }

    QQmlComponent crop_component(&engine);
    crop_component.loadFromModule(
        QStringLiteral("Shadow.DirectStrokeContract"),
        QStringLiteral("PrecisionCropOverlay")
    );
    std::unique_ptr<QObject> crop(crop_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("interactionEnabled"), true},
        {QStringLiteral("aspectRatioLock"), 0.0},
        {QStringLiteral("width"), 400.0},
        {QStringLiteral("height"), 300.0},
    }));
    if (!crop) {
        std::cerr << crop_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QObject* const crop_cursor = crop->findChild<QObject*>(QStringLiteral("cropSurfaceCursor"));
    if (!require(
            crop->property("visible").toBool() && crop_cursor != nullptr
                && crop_cursor->property("cursorShape").toInt() == Qt::CrossCursor,
            "crop mode owns a visible cursor across the complete surface"
        )) {
        return EXIT_FAILURE;
    }
    editor.setStateBusy(true);
    drainBindings();
    if (!require(
            crop_cursor->property("cursorShape").toInt() == Qt::BusyCursor,
            "crop mode keeps a visible busy cursor while gestures are locked"
        )) {
        return EXIT_FAILURE;
    }
    editor.setStateBusy(false);
    drainBindings();

    QQuickWindow window;
    window.setGeometry(0, 0, 400, 300);
    QQuickItem preview;
    preview.setParentItem(window.contentItem());
    preview.setSize(QSizeF{400, 300});

    QQmlComponent picker_component(&engine);
    picker_component.loadFromModule(
        QStringLiteral("Shadow.DirectStrokeContract"),
        QStringLiteral("PrecisionCanvasPickerInput")
    );
    std::unique_ptr<QObject> picker_object(picker_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("previewItem"), QVariant::fromValue(&preview)},
        {QStringLiteral("previewContentRect"), QRectF{0, 0, 400, 300}},
        {QStringLiteral("previewFrameReady"), false},
        {QStringLiteral("readyPreviewGeneration"), QString{}},
        {QStringLiteral("displayScale"), 0.1},
        {QStringLiteral("levelZeroWidth"), 4'000.0},
        {QStringLiteral("levelZeroHeight"), 3'000.0},
        {QStringLiteral("interactionEnabled"), true},
        {QStringLiteral("width"), 400.0},
        {QStringLiteral("height"), 300.0},
    }));
    auto* const picker = qobject_cast<QQuickItem*>(picker_object.get());
    if (!picker) {
        std::cerr << picker_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    picker->setParentItem(window.contentItem());
    window.show();
    drainBindings();

    QObject* const brush_cursor = picker->findChild<QObject*>(QStringLiteral("retouchBrushCursor"));
    QObject* const smaller_brush =
        picker->findChild<QObject*>(QStringLiteral("retouchBrushSmallerShortcut"));
    QObject* const larger_brush =
        picker->findChild<QObject*>(QStringLiteral("retouchBrushLargerShortcut"));
    if (!require(
            brush_cursor != nullptr && smaller_brush != nullptr && larger_brush != nullptr
                && std::abs(brush_cursor->property("width").toDouble() - 3.6) < 0.01,
            "the retouch cursor scales its level-zero radius into the bounded preview"
        )
        || !require(
            QMetaObject::invokeMethod(smaller_brush, "activated")
                && editor.retouch_brush_radius == 17,
            "the conventional left-bracket shortcut reduces the next repair radius"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            std::abs(brush_cursor->property("width").toDouble() - 3.4) < 0.01
                && QMetaObject::invokeMethod(larger_brush, "activated")
                && editor.retouch_brush_radius == 18,
            "shortcut edits and the live cursor remain in one brush-session state"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();

    editor.setRetouchSourcePicking(true);
    sendMouse(window, QEvent::MouseButtonPress, QPointF{100, 120}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{100, 120}, Qt::LeftButton, Qt::NoButton);
    QObject* const sampled_source_marker =
        picker->findChild<QObject*>(QStringLiteral("retouchSampledSourceMarker"));
    if (!require(
            editor.sample_source_count == 1 && !editor.retouch_source_picking
                && sampled_source_marker != nullptr
                && sampled_source_marker->property("visible").toBool(),
            "explicit source-pick mode consumes one click and leaves a visible movable source"
        )) {
        return EXIT_FAILURE;
    }
    sendMouse(window, QEvent::MouseButtonPress, QPointF{100, 120}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{140, 150}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{140, 150}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.move_source_count > 0
                && std::abs(
                       editor.retouch_sampled_source.value(QStringLiteral("x")).toDouble() - 0.35
                   ) < 0.02
                && std::abs(
                       editor.retouch_sampled_source.value(QStringLiteral("y")).toDouble() - 0.5
                   ) < 0.02,
            "the sampled source marker directly repositions the next repair source"
        )) {
        return EXIT_FAILURE;
    }

    picker->setProperty("selectedRetouchIndex", 0);
    drainBindings();
    if (!require(
            !sampled_source_marker->property("visible").toBool(),
            "a selected authored repair shows only its exact donor overlay, not the next-source "
            "marker"
        )) {
        return EXIT_FAILURE;
    }
    picker->setProperty("selectedRetouchIndex", -1);
    drainBindings();

    sendMouse(window, QEvent::MouseButtonPress, QPointF{50, 70}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{50, 70}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.stroke_commit_count == 1 && editor.spot_commit_count == 0
                && editor.committed_points.size() == 1,
            "a click authors one ordered stroke region instead of reopening the legacy spot list"
        )) {
        return EXIT_FAILURE;
    }

    sendMouse(window, QEvent::MouseButtonPress, QPointF{60, 80}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{130, 95}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{220, 120}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{260, 125}, Qt::LeftButton, Qt::NoButton);

    if (!require(
            picker->property("visible").toBool() && editor.stroke_commit_count == 2
                && editor.committed_points.size() >= 3 && editor.committed_retouch_width == 4'000
                && editor.committed_retouch_height == 3'000,
            "retouch commits one batch with authoritative level-zero geometry"
        )) {
        return EXIT_FAILURE;
    }

    QObject* const active_coverage =
        picker->findChild<QObject*>(QStringLiteral("activeRetouchCoverage"));
    if (!require(
            active_coverage != nullptr
                && std::abs(active_coverage->property("radiusPixels").toDouble() - 1.8) < 0.01,
            "fit-view repair coverage preserves the exact 18px level-zero radius"
        )) {
        return EXIT_FAILURE;
    }

    sendMouse(window, QEvent::MouseButtonPress, QPointF{60, 80}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{130, 95}, Qt::NoButton, Qt::LeftButton);
    picker->setProperty("interactionEnabled", false);
    drainBindings();
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{180, 105}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.stroke_commit_count == 2,
            "hiding the brush during a drag cancels without committing"
        ))
        return EXIT_FAILURE;

    QObject* const input = picker->findChild<QObject*>(QStringLiteral("retouchStrokeInputArea"));
    QVariantList points;
    for (int i = 0; i < 700; ++i) {
        points.push_back(QVariantMap{{"x", i / 1000.0}, {"y", 0.25}});
    }
    QVariant simplified;
    if (!require(
            input
                && QMetaObject::invokeMethod(
                    input,
                    "compactPath",
                    Q_RETURN_ARG(QVariant, simplified),
                    Q_ARG(QVariant, QVariant(points))
                ),
            "invoke long-path contract"
        ))
        return EXIT_FAILURE;
    const auto compacted = simplified.value<QJSValue>().isArray()
                               ? simplified.value<QJSValue>().toVariant().toList()
                               : simplified.toList();
    if (!require(
            compacted.size() == 2 && compacted.front().toMap() == points.front().toMap()
                && compacted.back().toMap() == points.back().toMap(),
            "long straight stroke keeps exact endpoints within error budget"
        ))
        return EXIT_FAILURE;

    QQmlComponent mask_component(&engine);
    mask_component.loadFromModule(
        QStringLiteral("Shadow.DirectStrokeContract"),
        QStringLiteral("PrecisionLocalMaskOverlay")
    );
    std::unique_ptr<QObject> mask_object(mask_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("interactionEnabled"), true},
        {QStringLiteral("nativeCoverageReady"), false},
        {QStringLiteral("coverageVisible"), true},
        {QStringLiteral("width"), 400.0},
        {QStringLiteral("height"), 300.0},
    }));
    auto* const mask = qobject_cast<QQuickItem*>(mask_object.get());
    if (!mask) {
        std::cerr << mask_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    mask->setParentItem(window.contentItem());
    drainBindings();

    sendMouse(window, QEvent::MouseButtonPress, QPointF{70, 180}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{140, 185}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{240, 200}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{290, 205}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.mask_commit_count == 1 && editor.committed_mask_points.size() >= 3,
            "mask painting renders locally and commits one bounded stroke"
        )) {
        return EXIT_FAILURE;
    }

    mask->setProperty("interactionEnabled", false);
    FakeLiquifyPreview liquify_preview;
    QQmlComponent liquify_component(&engine);
    liquify_component.loadFromModule(
        QStringLiteral("Shadow.DirectStrokeContract"),
        QStringLiteral("PrecisionLiquifyOverlay")
    );
    std::unique_ptr<QObject> liquify_object(liquify_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("previewItem"), QVariant::fromValue(&liquify_preview)},
        {QStringLiteral("interactionEnabled"), true},
        {QStringLiteral("previewReady"), true},
        {QStringLiteral("previewGeneration"), QStringLiteral("21")},
        {QStringLiteral("outputAspectRatio"), 4.0 / 3.0},
        {QStringLiteral("width"), 400.0},
        {QStringLiteral("height"), 300.0},
    }));
    auto* const liquify = qobject_cast<QQuickItem*>(liquify_object.get());
    if (!liquify) {
        std::cerr << liquify_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    liquify->setParentItem(window.contentItem());
    drainBindings();

    sendMouse(window, QEvent::MouseButtonPress, QPointF{80, 70}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{150, 90}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{240, 140}, Qt::NoButton, Qt::LeftButton);
    if (!require(
            editor.liquify_commit_count == 0,
            "Liquify pointer samples remain transient until gesture completion"
        )
        || !require(
            liquify_preview.begin_count == 1 && liquify_preview.append_count >= 3
                && liquify_preview.finish_count == 0
                && liquify_preview.begin_radius == editor.liquifyBrushRadius()
                && liquify_preview.begin_strength == editor.liquifyBrushStrength()
                && liquify_preview.begin_hardness == editor.liquifyBrushHardness()
                && liquify_preview.last_pressure == 1.0,
            "Liquify forwards bounded pressure-bearing samples only to the transient preview"
        )) {
        return EXIT_FAILURE;
    }
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{300, 165}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.liquify_commit_count == 1 && editor.committed_liquify_points.size() >= 3
                && editor.committed_liquify_aspect == 4.0 / 3.0,
            "one Liquify drag crosses the controller boundary exactly once"
        )
        || !require(
            editor.committed_liquify_points.constFirst()
                    .toMap()
                    .value(QStringLiteral("pressure"))
                    .toDouble()
                == 1.0,
            "mouse input records canonical full pressure while pressure-capable pointers remain "
            "authored"
        )
        || !require(
            liquify_preview.finish_count == 1 && liquify_preview.finish_committed
                && liquify_preview.cancel_count == 0,
            "the committed gesture remains on the GPU mesh until its authoritative frame arrives"
        )) {
        return EXIT_FAILURE;
    }
    QObject* const liquify_input =
        liquify->findChild<QObject*>(QStringLiteral("liquifyStrokeInput"));
    QObject* const liquify_pending_cursor =
        liquify->findChild<QObject*>(QStringLiteral("liquifyPendingCursor"));
    QObject* const liquify_brush_cursor =
        liquify->findChild<QObject*>(QStringLiteral("liquifyBrushCursor"));
    QObject* const liquify_outer_ring =
        liquify->findChild<QObject*>(QStringLiteral("liquifyBrushOuterRing"));
    QObject* const liquify_hardness_ring =
        liquify->findChild<QObject*>(QStringLiteral("liquifyBrushHardnessRing"));
    if (!require(
            liquify_input != nullptr && liquify_pending_cursor != nullptr
                && liquify_input->property("visible").toBool()
                && liquify_input->property("enabled").toBool()
                && !liquify_pending_cursor->property("visible").toBool(),
            "a pending local Push remains visible without blocking the next Liquify gesture"
        )
        || !require(
            liquify_brush_cursor != nullptr && liquify_brush_cursor->property("visible").toBool()
                && liquify_outer_ring != nullptr && liquify_hardness_ring != nullptr
                && std::abs(liquify_outer_ring->property("width").toDouble() - 48.0) < 0.01
                && std::abs(liquify_hardness_ring->property("width").toDouble() - 19.2) < 0.01,
            "Liquify feedback is a radius and hardness brush cursor over the live image"
        )) {
        return EXIT_FAILURE;
    }

    editor.setLiquifyBrushMode(1);
    drainBindings();
    const int transient_begin_count = liquify_preview.begin_count;
    sendMouse(window, QEvent::MouseButtonPress, QPointF{160, 120}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{190, 135}, Qt::NoButton, Qt::LeftButton);
    liquify->setProperty("previewReady", false);
    drainBindings();
    sendMouse(window, QEvent::MouseMove, QPointF{230, 145}, Qt::NoButton, Qt::LeftButton);
    if (!require(
            editor.liquify_live_begin_count == 1 && editor.liquify_live_kind == 1
                && editor.liquify_live_update_count >= 3 && editor.liquify_live_finish_count == 0
                && editor.liquify_live_cancel_count == 0
                && liquify_preview.begin_count == transient_begin_count,
            "Reconstruct survives a preview-readiness transition while streaming one authoritative "
            "path without restarting the Push mesh"
        )) {
        return EXIT_FAILURE;
    }
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{250, 150}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.liquify_live_finish_count == 1 && editor.liquify_live_cancel_count == 0
                && editor.committed_liquify_points.size() >= 3
                && editor.committed_liquify_aspect == 4.0 / 3.0,
            "Reconstruct release commits the streamed path as one ordered operation even when its "
            "first frame changes source readiness"
        )) {
        return EXIT_FAILURE;
    }
    liquify->setProperty("previewReady", true);
    liquify_preview.publishAuthoritativeFrame();
    drainBindings();

    // Releasing an authoritative stroke schedules one newer generation. Until
    // it presents, a following Push must stay on the authoritative path rather
    // than applying a new local mesh over a stale texture.
    editor.setLiquifyBrushMode(0);
    const int transient_begin_before_pending_push = liquify_preview.begin_count;
    const int live_begin_before_pending_push = editor.liquify_live_begin_count;
    sendMouse(window, QEvent::MouseButtonPress, QPointF{75, 185}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{130, 195}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{185, 205}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            liquify_preview.begin_count == transient_begin_before_pending_push
                && editor.liquify_live_begin_count == live_begin_before_pending_push + 1
                && editor.liquify_live_kind == 0,
            "a Push following an unsettled authoritative stroke cannot rebase a local mesh over "
            "the stale generation"
        )) {
        return EXIT_FAILURE;
    }
    liquify->setProperty("previewGeneration", QStringLiteral("22"));
    drainBindings();

    // A CPU/fallback source can display a decoded preview without admitting a
    // local Scene Graph texture. Push must still preview through the same
    // authoritative live-stroke lifecycle instead of silently drawing nothing.
    liquify_preview.begin_allowed = false;
    const int live_begin_before_fallback = editor.liquify_live_begin_count;
    const int live_finish_before_fallback = editor.liquify_live_finish_count;
    sendMouse(window, QEvent::MouseButtonPress, QPointF{90, 210}, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseMove, QPointF{150, 220}, Qt::NoButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, QPointF{220, 230}, Qt::LeftButton, Qt::NoButton);
    if (!require(
            editor.liquify_live_begin_count == live_begin_before_fallback + 1
                && editor.liquify_live_kind == 0
                && editor.liquify_live_finish_count == live_finish_before_fallback + 1
                && editor.liquify_live_cancel_count == 0,
            "Push falls back to authoritative live preview when the local display mesh is "
            "unavailable"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_direct_stroke_interaction_test.moc"
