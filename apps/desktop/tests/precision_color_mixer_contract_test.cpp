#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QTest>
#include <QVariant>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class ColorMixerEditorStub final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active MEMBER active CONSTANT)
    Q_PROPERTY(bool gradeNodeEnabled MEMBER grade_node_enabled CONSTANT)
    Q_PROPERTY(quint64 parameterRevision READ parameterRevision NOTIFY parametersChanged)

  public:
    [[nodiscard]] quint64 parameterRevision() const noexcept {
        return parameter_revision;
    }

    Q_INVOKABLE double colorMixerValue(const int band, const QString& component) const {
        if (band < 0 || band >= static_cast<int>(hue.size())) {
            return 0.0;
        }
        const auto index = static_cast<std::size_t>(band);
        if (component == QStringLiteral("hue")) {
            return hue[index];
        }
        if (component == QStringLiteral("saturation")) {
            return chroma[index];
        }
        return lightness[index];
    }

    Q_INVOKABLE void
    setColorMixerValue(const int band, const QString& component, const double value) {
        if (band < 0 || band >= static_cast<int>(hue.size())) {
            return;
        }
        const auto index = static_cast<std::size_t>(band);
        if (component == QStringLiteral("hue")) {
            hue[index] = value;
        } else if (component == QStringLiteral("saturation")) {
            chroma[index] = value;
        } else {
            lightness[index] = value;
        }
        ++set_count;
        ++parameter_revision;
        emit parametersChanged();
    }

    Q_INVOKABLE void beginParameterEdit(const QString&) {
        ++begin_count;
    }

    Q_INVOKABLE void endParameterEdit(const QString&) {
        ++end_count;
    }

    Q_INVOKABLE void resetSelectedAdjustmentSection(const QString&) {}

    bool active = true;
    bool grade_node_enabled = true;
    std::array<double, 8> hue{-0.25, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 8> chroma{0.35, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 8> lightness{0.10, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    quint64 parameter_revision = 0;
    int begin_count = 0;
    int set_count = 0;
    int end_count = 0;

  signals:
    void parametersChanged();
    void selectedGradeNodeChanged();
};

namespace {

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision Color Mixer contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] QQuickItem* findVisualChild(QQuickItem* const root, const QString& object_name) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == object_name) {
        return root;
    }
    for (QQuickItem* const child : root->childItems()) {
        if (auto* const match = findVisualChild(child, object_name)) {
            return match;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.PrecisionColorMixerContract"),
        QStringLiteral("PrecisionColorMixer")
    );

    ColorMixerEditorStub editor;
    std::unique_ptr<QObject> mixer{component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("panelRaised"), QColor{QStringLiteral("#20252b")}},
        {QStringLiteral("panelBorder"), QColor{QStringLiteral("#3a424b")}},
        {QStringLiteral("textPrimary"), QColor{QStringLiteral("#f2f4f6")}},
        {QStringLiteral("textMuted"), QColor{QStringLiteral("#9ca6af")}},
        {QStringLiteral("accent"), QColor{QStringLiteral("#65a8e8")}},
        {QStringLiteral("width"), 420.0},
    })};
    auto* const mixer_item = qobject_cast<QQuickItem*>(mixer.get());
    if (!mixer_item) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.resize(460, 380);
    mixer_item->setParentItem(window.contentItem());
    mixer_item->setPosition(QPointF{20.0, 12.0});
    mixer_item->setHeight(340.0);
    window.show();
    drainBindings();

    auto* const hue_tab = mixer->findChild<QQuickItem*>(QStringLiteral("colorMixerHueTab"));
    auto* const chroma_tab = mixer->findChild<QQuickItem*>(QStringLiteral("colorMixerChromaTab"));
    auto* const lightness_tab =
        mixer->findChild<QQuickItem*>(QStringLiteral("colorMixerLightnessTab"));
    if (!require(
            hue_tab != nullptr && chroma_tab != nullptr && lightness_tab != nullptr,
            "OKLCH component tabs expose real interaction targets"
        )) {
        return EXIT_FAILURE;
    }
    auto click_tab = [&](QQuickItem* const tab) {
        const QPointF center =
            tab->mapToItem(window.contentItem(), QPointF{tab->width() / 2.0, tab->height() / 2.0});
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
        drainBindings();
    };

    click_tab(lightness_tab);
    auto* const lightness_slider =
        findVisualChild(mixer_item, QStringLiteral("colorMixerBandSlider_lightness_0"));
    if (!require(lightness_slider != nullptr, "Lightness tab presents its first color band")
        || !require(
            std::abs(lightness_slider->property("value").toDouble() - 0.10) < 0.000'001,
            "Lightness tab reads the independent Lightness value"
        )
        || !require(
            editor.begin_count == 0 && editor.set_count == 0 && editor.end_count == 0,
            "switching OKLCH tabs causes no Recipe mutation or undo gesture"
        )) {
        return EXIT_FAILURE;
    }

    auto* const lightness_handle =
        lightness_slider->findChild<QQuickItem*>(QStringLiteral("shadowInlineSliderHandle"));
    if (!require(lightness_handle != nullptr, "Lightness band exposes its real slider thumb")) {
        return EXIT_FAILURE;
    }
    const QPointF handle_center = lightness_handle->mapToItem(
        window.contentItem(),
        QPointF{lightness_handle->width() / 2.0, lightness_handle->height() / 2.0}
    );
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, handle_center.toPoint());
    QTest::mouseMove(&window, (handle_center + QPointF{36.0, 0.0}).toPoint(), 40);
    QTest::mouseRelease(
        &window,
        Qt::LeftButton,
        Qt::NoModifier,
        (handle_center + QPointF{36.0, 0.0}).toPoint()
    );
    drainBindings();
    if (!require(
            editor.begin_count == 1 && editor.set_count >= 1 && editor.end_count == 1,
            "a real Lightness drag remains one authored adjustment gesture"
        )) {
        return EXIT_FAILURE;
    }
    const int begins_after_drag = editor.begin_count;
    const int sets_after_drag = editor.set_count;
    const int ends_after_drag = editor.end_count;

    click_tab(chroma_tab);
    auto* const chroma_slider =
        findVisualChild(mixer_item, QStringLiteral("colorMixerBandSlider_saturation_0"));
    if (!require(chroma_slider != nullptr, "Chroma tab presents its first color band")
        || !require(
            std::abs(chroma_slider->property("value").toDouble() - 0.35) < 0.000'001,
            "Chroma tab does not inherit the authored Lightness value"
        )) {
        return EXIT_FAILURE;
    }

    click_tab(hue_tab);
    auto* const hue_slider =
        findVisualChild(mixer_item, QStringLiteral("colorMixerBandSlider_hue_0"));
    return require(hue_slider != nullptr, "Hue tab presents its first color band")
                   && require(
                       std::abs(hue_slider->property("value").toDouble() + 0.25) < 0.000'001,
                       "Hue tab restores its independent authored value"
                   )
                   && require(
                       editor.begin_count == begins_after_drag
                           && editor.set_count == sets_after_drag
                           && editor.end_count == ends_after_drag,
                       "post-edit OKLCH tab changes remain presentation-only"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "precision_color_mixer_contract_test.moc"
