#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
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
    Q_PROPERTY(QVariantList pointColors READ pointColors CONSTANT)
    Q_PROPERTY(
        int selectedPointColorIndex
        READ selectedPointColorIndex
        WRITE setSelectedPointColorIndex
        NOTIFY parametersChanged
    )
    Q_PROPERTY(
        bool pointColorPickerActive
        READ pointColorPickerActive
        WRITE setPointColorPickerActive
        NOTIFY pointColorPickerActiveChanged
    )
    Q_PROPERTY(
        bool pointColorScopeActive
        READ pointColorScopeActive
        WRITE setPointColorScopeActive
        NOTIFY parametersChanged
    )
    Q_PROPERTY(
        bool pointColorScopeAvailable
        READ pointColorScopeAvailable
        WRITE setPointColorScopeAvailable
        NOTIFY parametersChanged
    )
    Q_PROPERTY(int parameterRevision READ parameterRevision NOTIFY parametersChanged)

public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept { return true; }
    [[nodiscard]] bool stateBusy() const noexcept { return false; }
    [[nodiscard]] QVariantList pointColors() const { return {}; }
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
    [[nodiscard]] int parameterRevision() const noexcept { return revision_; }

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
    Q_INVOKABLE void setParameterValue(
        const QString& key,
        const double value
    ) {
        set_key_ = key;
        hue_ = value;
    }
    Q_INVOKABLE void beginParameterEdit(const QString& key) {
        begin_key_ = key;
    }
    Q_INVOKABLE void endParameterEdit(const QString& key) {
        end_key_ = key;
    }
    Q_INVOKABLE void selectPointColor(int) {}
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
};

class FakeAnalysisScope final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int scopeMode MEMBER scope_mode_)
    Q_PROPERTY(int vectorscopeScope MEMBER vectorscope_scope_ CONSTANT)
    Q_PROPERTY(bool skinGuideVisible MEMBER skin_guide_visible_)
    Q_PROPERTY(
        double displayScopeSkinGuideDeviation
        MEMBER skin_guide_deviation_
        CONSTANT
    )
    Q_PROPERTY(
        int displayScopeMatchedPixels
        MEMBER matched_pixels_
        CONSTANT
    )
    Q_PROPERTY(
        bool displayScopeCentroidAvailable
        MEMBER centroid_available_
        CONSTANT
    )

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
        std::cerr
            << "Precision Point Color contract failed: "
            << message
            << '\n';
    }
    return condition;
}

[[nodiscard]] bool invoke(
    QObject* target,
    const char* method,
    const QVariant& argument
) {
    return QMetaObject::invokeMethod(
        target,
        method,
        Q_ARG(QVariant, argument)
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    const QString source_path = QStringLiteral(
        SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionPointColorSection.qml"
    );
    QQmlComponent component(&engine, QUrl::fromLocalFile(source_path));
    FakePointColorEditor editor;
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
    }));
    if (!section) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    if (!require(
            section->property("pickerAvailable").toBool(),
            "picker admission combines editor, frame, generation and comparison state"
        )
        || !require(
            QMetaObject::invokeMethod(section.get(), "toggleSkinCheck"),
            "skin-check lifecycle is invokable"
        )
        || !require(
            section->property("skinCheckPending").toBool()
                && editor.pointColorPickerActive()
                && analysis_scope.scope_mode_
                    == analysis_scope.vectorscope_scope_
                && analysis_scope.skin_guide_visible_,
            "unavailable scope enters one pending sample lifecycle"
        )) {
        return EXIT_FAILURE;
    }

    editor.setSelectedPointColorIndex(0);
    QCoreApplication::processEvents();
    if (!require(
            !section->property("skinCheckPending").toBool()
                && editor.pointColorScopeActive(),
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
            editor.begin_key_
                    == QStringLiteral("skin_guide/point_color_hue")
                && editor.set_key_ == QStringLiteral("color_range_hue")
                && editor.hue_ == 180.0
                && editor.end_key_
                    == QStringLiteral("skin_guide/point_color_hue"),
            "skin-guide correction is clamped and forms one undoable gesture"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_point_color_section_contract_test.moc"
