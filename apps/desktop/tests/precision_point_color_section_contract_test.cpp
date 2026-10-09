#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakePointColorEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool stateBusy READ stateBusy CONSTANT)
    Q_PROPERTY(QVariantList pointColors READ pointColors NOTIFY parametersChanged)
    Q_PROPERTY(
        int selectedPointColorIndex READ selectedPointColorIndex WRITE setSelectedPointColorIndex
            NOTIFY parametersChanged
    )
    Q_PROPERTY(
        bool pointColorPickerActive READ pointColorPickerActive WRITE setPointColorPickerActive
            NOTIFY pointColorPickerActiveChanged
    )
    Q_PROPERTY(
        bool pointColorScopeActive READ pointColorScopeActive WRITE setPointColorScopeActive NOTIFY
            parametersChanged
    )
    Q_PROPERTY(
        bool pointColorScopeAvailable READ pointColorScopeAvailable WRITE
            setPointColorScopeAvailable NOTIFY parametersChanged
    )
    Q_PROPERTY(int parameterRevision READ parameterRevision NOTIFY parametersChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool stateBusy() const noexcept {
        return false;
    }
    [[nodiscard]] QVariantList pointColors() const {
        return point_colors_;
    }
    [[nodiscard]] int selectedPointColorIndex() const noexcept {
        return selected_index_;
    }
    [[nodiscard]] bool pointColorPickerActive() const noexcept {
        return picker_active_;
    }
    [[nodiscard]] bool pointColorScopeActive() const noexcept {
        return scope_active_;
    }
    [[nodiscard]] bool pointColorScopeAvailable() const noexcept {
        return scope_available_;
    }
    [[nodiscard]] int parameterRevision() const noexcept {
        return revision_;
    }

    void setSelectedPointColorIndex(const int index) {
        selected_index_ = index;
        ++revision_;
        emit parametersChanged();
    }
    Q_INVOKABLE void setPointColorPickerActive(const bool active) {
        if (picker_active_ == active) {
            return;
        }
        picker_active_ = active;
        emit pointColorPickerActiveChanged();
    }
    void setPointColorScopeActive(const bool active) {
        scope_active_ = active;
        emit parametersChanged();
    }
    void setPointColorScopeAvailable(const bool available) {
        scope_available_ = available;
        emit parametersChanged();
    }

    Q_INVOKABLE double parameterValue(const QString& key) const {
        return key == QStringLiteral("color_range_hue") ? hue_ : 0.0;
    }
    Q_INVOKABLE void setParameterValue(const QString& key, const double value) {
        set_key_ = key;
        hue_ = value;
    }
    Q_INVOKABLE void beginParameterEdit(const QString& key) {
        begin_key_ = key;
    }
    Q_INVOKABLE void endParameterEdit(const QString& key) {
        end_key_ = key;
    }
    void publishPointColors(const int count) {
        point_colors_.clear();
        for (int index = 0; index < count; ++index) {
            point_colors_.append(
                QVariantMap{
                    {QStringLiteral("index"), index},
                    {
                        QStringLiteral("swatch"),
                        QColor::fromHsl((index * 31) % 360, 160, 150),
                    },
                }
            );
        }
        emit parametersChanged();
    }
    Q_INVOKABLE void selectPointColor(const int index) {
        setSelectedPointColorIndex(index);
    }
    Q_INVOKABLE void removeSelectedPointColor() {}

    double hue_ = 0.0;
    QString begin_key_;
    QString set_key_;
    QString end_key_;

  signals:
    void parametersChanged();
    void pointColorPickerActiveChanged();
    void selectedGradeNodeChanged();

  private:
    int selected_index_ = -1;
    int revision_ = 0;
    bool picker_active_ = false;
    bool scope_active_ = false;
    bool scope_available_ = false;
    QVariantList point_colors_{
        QVariantMap{
            {QStringLiteral("index"), 0},
            {QStringLiteral("swatch"), QColor(QStringLiteral("#d86a52"))},
        },
        QVariantMap{
            {QStringLiteral("index"), 1},
            {QStringLiteral("swatch"), QColor(QStringLiteral("#4e78c4"))},
        },
    };
};

class FakeAnalysisScope final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int scopeMode MEMBER scope_mode_)
    Q_PROPERTY(int vectorscopeScope MEMBER vectorscope_scope_ CONSTANT)
    Q_PROPERTY(bool skinGuideVisible MEMBER skin_guide_visible_)
    Q_PROPERTY(double displayScopeSkinGuideDeviation MEMBER skin_guide_deviation_ CONSTANT)
    Q_PROPERTY(int displayScopeMatchedPixels MEMBER matched_pixels_ CONSTANT)
    Q_PROPERTY(bool displayScopeCentroidAvailable MEMBER centroid_available_ CONSTANT)

  public:
    using QObject::QObject;

    Q_INVOKABLE QVariantMap skinToneRange(const QString&) const {
        return {
            {QStringLiteral("available"), false},
            {QStringLiteral("deviation"), 0.0},
        };
    }

    int scope_mode_ = 0;
    int vectorscope_scope_ = 2;
    bool skin_guide_visible_ = false;
    double skin_guide_deviation_ = 0.0;
    int matched_pixels_ = 0;
    bool centroid_available_ = false;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision Point Color contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(QObject* target, const char* method, const QVariant& argument) {
    return QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, argument));
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] QQuickItem* findSample(QQuickItem* const root, const int sample_position) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == QStringLiteral("pointColorSwatch")
        && root->property("samplePosition").toInt() == sample_position) {
        return root;
    }
    for (QQuickItem* const child : root->childItems()) {
        if (QQuickItem* const match = findSample(child, sample_position); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    FakePointColorEditor editor;

    const QString histogram_source_path =
        QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/EditHistogram.qml");
    QQmlComponent histogram_component(&engine, QUrl::fromLocalFile(histogram_source_path));
    const QVariantMap histogram_analysis{
        {QStringLiteral("displayScopeSkinShadowsAvailable"), true},
        {QStringLiteral("displayScopeSkinShadowsMatchedPixels"), 128},
        {
            QStringLiteral("displayScopeSkinShadowsDeviationDegrees"),
            7.5,
        },
    };
    std::unique_ptr<QObject> histogram(histogram_component.createWithInitialProperties({
        {QStringLiteral("analysis"), histogram_analysis},
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {
            QStringLiteral("displayGeneration"),
            QStringLiteral("generation-a"),
        },
    }));
    QVariant shadows;
    if (!histogram
        || !QMetaObject::invokeMethod(
            histogram.get(),
            "skinToneRange",
            Q_RETURN_ARG(QVariant, shadows),
            Q_ARG(QVariant, QStringLiteral("Shadows"))
        )) {
        std::cerr << histogram_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    const QVariantMap shadows_map = shadows.toMap();
    if (!require(
            shadows_map.value(QStringLiteral("available")).toBool()
                && shadows_map.value(QStringLiteral("matchedPixels")).toInt() == 128
                && shadows_map.value(QStringLiteral("deviation")).toDouble() == 7.5,
            "histogram forwards the complete skin-tone range contract"
        )) {
        return EXIT_FAILURE;
    }

    const QString source_path =
        QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionPointColorSection.qml");
    QQmlComponent component(&engine, QUrl::fromLocalFile(source_path));
    FakeAnalysisScope analysis_scope;
    std::unique_ptr<QObject> section(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {
            QStringLiteral("analysisScope"),
            QVariant::fromValue(&analysis_scope),
        },
        {QStringLiteral("previewFrameReady"), true},
        {
            QStringLiteral("readyPreviewGeneration"),
            QStringLiteral("generation-a"),
        },
        {QStringLiteral("comparisonActive"), false},
        {QStringLiteral("accent"), QColor(QStringLiteral("#3d9cff"))},
        {QStringLiteral("width"), 230.0},
        {QStringLiteral("height"), 900.0},
    }));
    if (!section) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QQuickWindow focus_window;
    focus_window.setGeometry(0, 0, 230, 900);
    auto* const section_item = qobject_cast<QQuickItem*>(section.get());
    if (section_item != nullptr) {
        section_item->setParentItem(focus_window.contentItem());
    }
    focus_window.show();
    focus_window.requestActivate();
    drainBindings();

    QObject* sample_bar = section->findChild<QObject*>(QStringLiteral("pointColorSampleBar"));
    QObject* swatch_repeater =
        section->findChild<QObject*>(QStringLiteral("pointColorSwatchRepeater"));
    QObject* guidance = section->findChild<QObject*>(QStringLiteral("pointColorPickerGuidance"));
    if (!require(
            sample_bar != nullptr && swatch_repeater != nullptr
                && swatch_repeater->property("count").toInt() == 2
                && sample_bar->property("swatchTargetSize").toInt() == 36,
            "sample swatches remain keyboard-focusable 36-pixel targets"
        )
        || !require(
            guidance != nullptr && !guidance->property("text").toString().isEmpty(),
            "the picker always exposes concise visible state guidance"
        )
        || !require(
            section->property("pickerAvailable").toBool(),
            "picker admission combines editor, frame, generation and comparison state"
        )
        || !require(
            QMetaObject::invokeMethod(section.get(), "toggleSkinCheck"),
            "skin-check lifecycle is invokable"
        )
        || !require(
            section->property("skinCheckPending").toBool() && editor.pointColorPickerActive()
                && analysis_scope.scope_mode_ == analysis_scope.vectorscope_scope_
                && analysis_scope.skin_guide_visible_,
            "unavailable scope enters one pending sample lifecycle"
        )) {
        return EXIT_FAILURE;
    }

    editor.setSelectedPointColorIndex(0);
    QCoreApplication::processEvents();
    if (!require(
            !section->property("skinCheckPending").toBool() && editor.pointColorScopeActive(),
            "a completed point sample locks the requested skin diagnostic"
        )
        || !require(
            QMetaObject::invokeMethod(section.get(), "toggleSkinCheck")
                && !editor.pointColorScopeActive(),
            "the same action releases an active diagnostic"
        )) {
        return EXIT_FAILURE;
    }

    editor.hue_ = 175.0;
    if (!require(
            invoke(section.get(), "applySkinGuideNudge", 12.0),
            "skin-guide nudge is invokable"
        )
        || !require(
            editor.begin_key_ == QStringLiteral("skin_guide/point_color_hue")
                && editor.set_key_ == QStringLiteral("color_range_hue") && editor.hue_ == 180.0
                && editor.end_key_ == QStringLiteral("skin_guide/point_color_hue"),
            "skin-guide correction is clamped and forms one undoable gesture"
        )) {
        return EXIT_FAILURE;
    }

    editor.publishPointColors(12);
    editor.setSelectedPointColorIndex(11);
    drainBindings();
    QObject* const swatch_flickable =
        section->findChild<QObject*>(QStringLiteral("pointColorSwatchFlickable"));
    if (swatch_flickable != nullptr) {
        // Resize the unowned host/root; the Flickable width belongs to its layout.
        focus_window.setWidth(220);
        section->setProperty("width", 220.0);
        drainBindings();
        QMetaObject::invokeMethod(sample_bar, "revealSelectedSwatch");
        drainBindings();
    }
    if (!require(
            swatch_flickable != nullptr && sample_bar->property("focusRingInset").toInt() == 2
                && swatch_flickable->property("contentWidth").toDouble()
                       > swatch_flickable->property("width").toDouble()
                && swatch_flickable->property("contentX").toDouble() > 0.0
                && swatch_flickable->property("contentX").toDouble()
                           + swatch_flickable->property("width").toDouble()
                       >= swatch_flickable->property("contentWidth").toDouble() - 0.5,
            "the current sample is auto-revealed while its focus ring remains inside clipping"
        )
        || !require(
            sample_bar->property("selectedSamplePosition").toInt() == 11,
            "the selected sample exposes one stable selected state"
        )) {
        return EXIT_FAILURE;
    }

    editor.setSelectedPointColorIndex(0);
    drainBindings();
    swatch_flickable->setProperty("contentX", 0.0);
    QQuickItem* const nonselected_last_sample = findSample(section_item, 11);
    if (nonselected_last_sample != nullptr) {
        nonselected_last_sample->forceActiveFocus(Qt::TabFocusReason);
    }
    // A deferred selection reveal can arrive after keyboard focus, for example
    // when the row width changes during layout. It must not hide the focused swatch.
    if (!require(
            swatch_flickable->property("contentX").toDouble() > 0.0
                && QMetaObject::invokeMethod(sample_bar, "queueRevealSelectedSwatch"),
            "keyboard focus reveals its target before a queued selection refresh"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    if (!require(
            nonselected_last_sample != nullptr && nonselected_last_sample->hasActiveFocus()
                && !nonselected_last_sample->property("selected").toBool()
                && sample_bar->property("selectedSamplePosition").toInt() == 0,
            "a real Tab-reason focus may move to an unselected sample"
        )
        || !require(
            swatch_flickable->property("contentX").toDouble() > 0.0
                && swatch_flickable->property("contentX").toDouble()
                           + swatch_flickable->property("width").toDouble()
                       >= swatch_flickable->property("contentWidth").toDouble() - 0.5,
            "keyboard focus reveals the focused delegate rather than the selected sample"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_point_color_section_contract_test.moc"
