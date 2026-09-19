#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QString>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

class FakeGeometryEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy CONSTANT)
    Q_PROPERTY(QVariantMap photoGeometry READ photoGeometry NOTIFY parametersChanged)
    Q_PROPERTY(bool autoGeometryBusy READ autoGeometryBusy NOTIFY autoGeometryChanged)
    Q_PROPERTY(bool autoGeometryCanAnalyze READ autoGeometryCanAnalyze NOTIFY autoGeometryChanged)
    Q_PROPERTY(bool autoGeometryHasProposal READ autoGeometryHasProposal NOTIFY autoGeometryChanged)
    Q_PROPERTY(bool autoGeometryPreviewing READ autoGeometryPreviewing NOTIFY autoGeometryChanged)
    Q_PROPERTY(int autoGeometryConfidence READ autoGeometryConfidence NOTIFY autoGeometryChanged)
    Q_PROPERTY(
        double autoGeometrySuggestedStraighten READ autoGeometrySuggestedStraighten NOTIFY
            autoGeometryChanged
    )
    Q_PROPERTY(
        double autoGeometrySuggestedVertical READ autoGeometrySuggestedVertical NOTIFY
            autoGeometryChanged
    )
    Q_PROPERTY(
        double autoGeometrySuggestedHorizontal READ autoGeometrySuggestedHorizontal NOTIFY
            autoGeometryChanged
    )
    Q_PROPERTY(
        int autoGeometrySupportingLines READ autoGeometrySupportingLines NOTIFY autoGeometryChanged
    )
    Q_PROPERTY(
        QString autoGeometryStatusText READ autoGeometryStatusText NOTIFY autoGeometryChanged
    )

  public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool stateBusy() const noexcept {
        return false;
    }
    [[nodiscard]] QVariantMap photoGeometry() const {
        return {
            {QStringLiteral("identity"), true},
            {QStringLiteral("quarterTurn"), quarter_turn_},
            {QStringLiteral("cropLeft"), 0.0},
            {QStringLiteral("cropTop"), 0.0},
            {QStringLiteral("cropRight"), crop_width_},
            {QStringLiteral("cropBottom"), crop_height_},
            {QStringLiteral("straightenDegrees"), 0.0},
            {QStringLiteral("perspectiveVertical"), 0.0},
            {QStringLiteral("perspectiveHorizontal"), 0.0},
        };
    }
    [[nodiscard]] bool autoGeometryBusy() const noexcept {
        return false;
    }
    [[nodiscard]] bool autoGeometryCanAnalyze() const noexcept {
        return !has_proposal_;
    }
    [[nodiscard]] bool autoGeometryHasProposal() const noexcept {
        return has_proposal_;
    }
    [[nodiscard]] bool autoGeometryPreviewing() const noexcept {
        return preview_ready_;
    }
    [[nodiscard]] int autoGeometryConfidence() const noexcept {
        return has_proposal_ ? 91 : 0;
    }
    [[nodiscard]] double autoGeometrySuggestedStraighten() const noexcept {
        return has_proposal_ ? 1.6 : 0.0;
    }
    [[nodiscard]] double autoGeometrySuggestedVertical() const noexcept {
        return has_proposal_ ? 0.08 : 0.0;
    }
    [[nodiscard]] double autoGeometrySuggestedHorizontal() const noexcept {
        return has_proposal_ ? -0.02 : 0.0;
    }
    [[nodiscard]] int autoGeometrySupportingLines() const noexcept {
        return has_proposal_ ? 14 : 0;
    }
    [[nodiscard]] QString autoGeometryStatusText() const {
        return has_proposal_ ? QStringLiteral("Proposal ready") : QString{};
    }

    void publishProposal() {
        has_proposal_ = true;
        preview_ready_ = false;
        emit autoGeometryChanged();
    }

    void publishPreviewReady() {
        preview_ready_ = true;
        emit autoGeometryChanged();
    }

    Q_INVOKABLE void analyzeAutoGeometry(const int mode) {
        ++analysis_count;
        last_analysis_mode = mode;
    }
    Q_INVOKABLE void acceptAutoGeometry() {
        ++accept_count;
    }
    Q_INVOKABLE void cancelAutoGeometry() {
        ++cancel_count;
    }
    Q_INVOKABLE void rotatePhotoCounterClockwise() {}
    Q_INVOKABLE void rotatePhotoClockwise() {}
    Q_INVOKABLE void flipPhotoHorizontally() {}
    Q_INVOKABLE void flipPhotoVertically() {}
    Q_INVOKABLE void resetPhotoGeometry() {}
    Q_INVOKABLE void beginParameterEdit(const QString&) {}
    Q_INVOKABLE void endParameterEdit(const QString&) {}
    Q_INVOKABLE void setPhotoStraightenDegrees(double) {}
    Q_INVOKABLE void setPhotoPerspective(double, double) {}
    Q_INVOKABLE void setCenteredPhotoCropAspectRatio(const double target, const double current) {
        requested_aspect = target;
        supplied_crop_aspect = current;
    }

    void setCropExtent(const double width, const double height) {
        crop_width_ = width;
        crop_height_ = height;
        emit parametersChanged();
    }

    void setQuarterTurn(const int turn) {
        quarter_turn_ = turn;
        emit parametersChanged();
    }

    int analysis_count = 0;
    int last_analysis_mode = -1;
    int accept_count = 0;
    int cancel_count = 0;
    double requested_aspect = 0;
    double supplied_crop_aspect = 0;

  signals:
    void parametersChanged();
    void autoGeometryChanged();

  private:
    int quarter_turn_ = 0;
    double crop_width_ = 1;
    double crop_height_ = 1;
    bool has_proposal_ = false;
    bool preview_ready_ = false;
};

class FakeGeometryInspector final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* editor READ editor CONSTANT)
    Q_PROPERTY(bool previewFrameReady READ previewFrameReady CONSTANT)
    Q_PROPERTY(double currentPhotoAspect READ currentPhotoAspect CONSTANT)

  public:
    explicit FakeGeometryInspector(QObject* editor) : editor_(editor) {}

    [[nodiscard]] QObject* editor() const noexcept {
        return editor_;
    }
    [[nodiscard]] bool previewFrameReady() const noexcept {
        return true;
    }
    [[nodiscard]] double currentPhotoAspect() const noexcept {
        const auto crop = editor_->property("photoGeometry").toMap();
        return crop.value(QStringLiteral("quarterTurn")).toInt() % 2 == 0 ? 1.5 : 2.0 / 3.0;
    }

  private:
    QObject* editor_;
};

namespace {

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool click(QObject& object) {
    const bool invoked = QMetaObject::invokeMethod(&object, "clicked");
    drainBindings();
    return invoked;
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision geometry-tools contract failed: " << message << '\n';
    }
    return condition;
}

QQuickItem* findVisualItem(QQuickItem* item, const QString& name) {
    if (item->objectName() == name)
        return item;
    for (auto* child : item->childItems()) {
        if (auto* found = findVisualItem(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    FakeGeometryEditor editor;
    FakeGeometryInspector inspector(&editor);
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionGeometryTools.qml")
        )
    );
    std::unique_ptr<QObject> tools(component.createWithInitialProperties({
        {QStringLiteral("inspector"), QVariant::fromValue(&inspector)},
        {QStringLiteral("currentTabIndex"), 0},
        {QStringLiteral("aspectRatioLock"), 0.0},
        {QStringLiteral("width"), 280.0},
    }));
    if (!tools) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    auto* const root_item = qobject_cast<QQuickItem*>(tools.get());
    auto* const automatic_button =
        tools->findChild<QObject*>(QStringLiteral("autoGeometryAutomaticButton"));
    auto* const proposal_label =
        tools->findChild<QObject*>(QStringLiteral("autoGeometryProposalLabel"));
    auto* const apply_button =
        tools->findChild<QObject*>(QStringLiteral("autoGeometryApplyButton"));
    auto* const cancel_button =
        tools->findChild<QObject*>(QStringLiteral("autoGeometryCancelButton"));
    if (!require(
            root_item != nullptr && automatic_button != nullptr && proposal_label != nullptr
                && apply_button != nullptr && cancel_button != nullptr,
            "the packaged geometry surface exposes analysis and proposal controls"
        )) {
        return EXIT_FAILURE;
    }

    drainBindings();
    // Wire the real output-space lock just as the workspace does. Rotation
    // preserves the crop instead of fitting a new centered rectangle.
    QQmlExpression connect_aspect(
        QQmlEngine::contextForObject(tools.get()), tools.get(),
        QStringLiteral("aspectRatioRequested.connect(function(r) { aspectRatioLock = r })")
    );
    connect_aspect.evaluate();
    tools->setProperty("aspectRatioLock", 0.8);
    drainBindings();
    auto* const portrait_button = findVisualItem(root_item, QStringLiteral("cropAspect4:5"));
    editor.setQuarterTurn(1);
    drainBindings();
    if (!require(
            portrait_button && tools->property("aspectRatioLock").toDouble() == 1.25
                && portrait_button->property("text").toString() == QStringLiteral("5:4")
                && portrait_button->property("selected").toBool(),
            "quarter turn reciprocates the handle lock and displays the actual output aspect"
        )) {
        return EXIT_FAILURE;
    }
    editor.setQuarterTurn(0);
    drainBindings();
    if (!require(
            tools->property("aspectRatioLock").toDouble() == 0.8
                && portrait_button->property("text").toString() == QStringLiteral("4:5")
                && portrait_button->property("selected").toBool(),
            "undoing a quarter turn restores the portrait constraint"
        )) {
        return EXIT_FAILURE;
    }
    tools->setProperty("aspectRatioLock", 0.0);
    editor.setQuarterTurn(3);
    drainBindings();
    if (!require(tools->property("aspectRatioLock").toDouble() == 0.0,
                 "freeform stays freeform across counterclockwise rotation")) {
        return EXIT_FAILURE;
    }
    editor.setQuarterTurn(0);
    editor.setCropExtent(0.6, 0.5);
    drainBindings();
    if (!require(click(*portrait_button) && editor.requested_aspect == 0.8
                     && qAbs(editor.supplied_crop_aspect - 1.8) < 0.000001,
                 "repeated aspect choice supplies the existing crop ratio, not the full image")) {
        return EXIT_FAILURE;
    }
    editor.setQuarterTurn(1);
    drainBindings();
    if (!require(click(*portrait_button) && editor.requested_aspect == 1.25
                     && qAbs(editor.supplied_crop_aspect - 1.0 / 1.8) < 0.000001,
                 "a rotated existing crop supplies its oriented rectangle ratio")) {
        return EXIT_FAILURE;
    }
    if (!require(
            click(*automatic_button) && editor.analysis_count == 1
                && editor.last_analysis_mode == 0,
            "the Auto action requests the deterministic automatic mode"
        )) {
        return EXIT_FAILURE;
    }

    editor.publishProposal();
    drainBindings();
    if (!require(
            proposal_label->property("visible").toBool()
                && proposal_label->property("text").toString().contains(QStringLiteral("14"))
                && apply_button->property("visible").toBool()
                && !apply_button->property("enabled").toBool()
                && cancel_button->property("visible").toBool(),
            "a proposal exposes evidence but cannot be accepted before its preview settles"
        )) {
        return EXIT_FAILURE;
    }

    editor.publishPreviewReady();
    drainBindings();
    if (!require(
            apply_button->property("enabled").toBool() && click(*apply_button)
                && click(*cancel_button) && editor.accept_count == 1 && editor.cancel_count == 1,
            "proposal actions reach the explicit accept and cancel lifecycle"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#include "precision_geometry_tools_contract_test.moc"
