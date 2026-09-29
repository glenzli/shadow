#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QTest>
#include <QUrl>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakePreviewEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active MEMBER active NOTIFY stateChanged)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY stateChanged)
    Q_PROPERTY(bool stateBusy MEMBER state_busy NOTIFY stateChanged)
    Q_PROPERTY(bool rendering MEMBER rendering NOTIFY stateChanged)
    Q_PROPERTY(bool fullResolutionPreparing MEMBER full_resolution_preparing NOTIFY stateChanged)
    Q_PROPERTY(bool detailMode MEMBER detail_mode NOTIFY stateChanged)
    Q_PROPERTY(bool detailRendering MEMBER detail_rendering NOTIFY stateChanged)
    Q_PROPERTY(bool beforeRendering MEMBER before_rendering NOTIFY stateChanged)
    Q_PROPERTY(QString detailErrorText MEMBER detail_error_text NOTIFY stateChanged)
    Q_PROPERTY(QString beforeErrorText MEMBER before_error_text NOTIFY stateChanged)
    Q_PROPERTY(QString statusText MEMBER status_text NOTIFY stateChanged)

  public:
    bool active = true;
    bool busy = false;
    bool state_busy = false;
    bool rendering = false;
    bool full_resolution_preparing = false;
    bool detail_mode = false;
    bool detail_rendering = false;
    bool before_rendering = false;
    QString detail_error_text;
    QString before_error_text;
    QString status_text;

  signals:
    void stateChanged();
};

namespace {

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision canvas status-overlays contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    FakePreviewEditor editor;

    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(
                SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionCanvasStatusOverlays.qml"
            )
        )
    );
    std::unique_ptr<QObject> overlays(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("comparisonActive"), false},
        {QStringLiteral("comparisonMode"), 0},
        {QStringLiteral("showingFullDetail"), false},
        {QStringLiteral("fitView"), true},
        {QStringLiteral("zoomFactor"), 1.0},
        {QStringLiteral("detailImageReady"), false},
        {QStringLiteral("detailImageLoadFailed"), false},
        {QStringLiteral("beforeReady"), false},
        {QStringLiteral("beforeFrameReady"), false},
        {QStringLiteral("previewFrameReady"), true},
        {QStringLiteral("previewLoadFailed"), false},
        {QStringLiteral("comparisonWhole"), 0},
        {QStringLiteral("comparisonWipeVertical"), 1},
        {QStringLiteral("comparisonWipeHorizontal"), 2},
        {QStringLiteral("comparisonSideBySide"), 3},
        {QStringLiteral("width"), 640.0},
        {QStringLiteral("height"), 420.0},
    }));
    if (!overlays) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QObject* const retained_busy_indicator = overlays->findChild<QObject*>(
        QStringLiteral("precisionRetainedPreviewBusyIndicator")
    );
    QObject* const retained_busy_rotor = overlays->findChild<QObject*>(
        QStringLiteral("precisionRetainedPreviewBusyIndicatorRotor")
    );
    QObject* const detail_wait_hint = overlays->findChild<QObject*>(
        QStringLiteral("precisionDetailWaitingHint")
    );
    QObject* const detail_wait_spinner = overlays->findChild<QObject*>(
        QStringLiteral("precisionDetailWaitSpinner")
    );
    QObject* const primary_wait_overlay = overlays->findChild<QObject*>(
        QStringLiteral("precisionPrimaryWaitOverlay")
    );
    if (!require(
            retained_busy_indicator != nullptr,
            "retained-preview pending state has a spinner affordance"
        )
        || !require(
            retained_busy_rotor != nullptr,
            "the retained-preview spinner exposes an explicitly animated rotor"
        )
        || !require(
            detail_wait_hint != nullptr && detail_wait_spinner != nullptr,
            "foreground full-detail waiting has a dedicated spinner affordance"
        )
        || !require(
            primary_wait_overlay != nullptr,
            "opening and first-render waiting owns a delayed foreground overlay"
        )
        || !require(
            !retained_busy_indicator->property("visible").toBool(),
            "a settled current preview does not show a busy affordance"
        )) {
        return EXIT_FAILURE;
    }

    editor.rendering = true;
    emit editor.stateChanged();
    drainBindings();
    if (!require(
            !retained_busy_indicator->property("visible").toBool(),
            "a fast retained-frame replacement does not flash a spinner"
        )) {
        return EXIT_FAILURE;
    }
    QTest::qWait(380);
    drainBindings();
    if (!require(
            retained_busy_indicator->property("visible").toBool(),
            "a perceptibly long retained-frame replacement shows the spinner"
        )) {
        return EXIT_FAILURE;
    }
    const double rotation_before = retained_busy_rotor->property("rotation").toDouble();
    QTest::qWait(120);
    drainBindings();
    if (!require(
            std::abs(
                retained_busy_rotor->property("rotation").toDouble() - rotation_before
            ) > 1.0,
            "a disclosed wait has visibly moving state instead of a static glyph"
        )) {
        return EXIT_FAILURE;
    }

    editor.rendering = false;
    editor.full_resolution_preparing = true;
    emit editor.stateChanged();
    drainBindings();
    if (!require(
            !retained_busy_indicator->property("visible").toBool(),
            "idle full-resolution warmup does not interrupt the overview surface"
        )
        || !require(
            !detail_wait_hint->property("visible").toBool(),
            "idle full-resolution warmup stays hidden while no detail surface is requested"
        )) {
        return EXIT_FAILURE;
    }
    QTest::qWait(380);
    drainBindings();
    if (!require(
            !retained_busy_indicator->property("visible").toBool(),
            "background warmup cannot acquire foreground wait state after the delay"
        )) {
        return EXIT_FAILURE;
    }

    overlays->setProperty("fitView", false);
    editor.detail_mode = true;
    emit editor.stateChanged();
    drainBindings();
    if (!require(
            detail_wait_hint->property("visible").toBool()
                && detail_wait_spinner->property("visible").toBool(),
            "the same full-resolution work is disclosed after the user requests 100% detail"
        )
        || !require(
            !retained_busy_indicator->property("visible").toBool(),
            "the foreground detail wait does not duplicate the overview spinner"
        )) {
        return EXIT_FAILURE;
    }

    overlays->setProperty("fitView", true);
    editor.detail_mode = false;
    editor.full_resolution_preparing = false;
    editor.state_busy = true;
    overlays->setProperty("previewFrameReady", false);
    emit editor.stateChanged();
    drainBindings();
    if (!require(
            !retained_busy_indicator->property("visible").toBool(),
            "the retained-frame spinner yields to the existing opening/rendering overlay"
        )
        || !require(
            !primary_wait_overlay->property("visible").toBool(),
            "a fast first render does not flash the primary wait overlay"
        )) {
        return EXIT_FAILURE;
    }
    QTest::qWait(280);
    drainBindings();
    if (!require(
            primary_wait_overlay->property("visible").toBool(),
            "a perceptibly long first render shows the animated primary wait overlay"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_canvas_status_overlays_contract_test.moc"
