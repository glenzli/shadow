#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FoundationEditorStub final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active MEMBER active CONSTANT)
    Q_PROPERTY(bool stateBusy MEMBER state_busy NOTIFY aiChanged)
    Q_PROPERTY(
        bool foundationAiDenoiseEnabled READ foundationAiDenoiseEnabled WRITE
            setFoundationAiDenoiseEnabled NOTIFY aiChanged
    )
    Q_PROPERTY(bool foundationAiDenoiseRequested MEMBER ai_requested NOTIFY aiChanged)
    Q_PROPERTY(bool rawDenoiseNodeVisible MEMBER raw_denoise_node_visible NOTIFY aiChanged)
    Q_PROPERTY(
        int foundationAiDenoiseAmount READ foundationAiDenoiseAmount WRITE
            setFoundationAiDenoiseAmount NOTIFY aiChanged
    )
    Q_PROPERTY(bool foundationAiDenoiseAvailable MEMBER ai_available NOTIFY aiChanged)
    Q_PROPERTY(bool foundationAiDenoiseBusy MEMBER ai_busy NOTIFY aiChanged)
    Q_PROPERTY(bool foundationAiDenoiseCanStart MEMBER ai_can_start NOTIFY aiChanged)
    Q_PROPERTY(bool foundationAiDenoiseCanApply MEMBER ai_can_apply NOTIFY aiChanged)
    Q_PROPERTY(bool foundationAiDenoiseCanCancel MEMBER ai_can_cancel NOTIFY aiChanged)
    Q_PROPERTY(QString foundationAiDenoisePhase MEMBER ai_phase NOTIFY aiChanged)
    Q_PROPERTY(double foundationAiDenoiseProgress MEMBER ai_progress NOTIFY aiChanged)
    Q_PROPERTY(QString foundationAiDenoiseStatusText MEMBER ai_status NOTIFY aiChanged)
    Q_PROPERTY(bool foundationAiDenoiseNoiseAssessmentBusy MEMBER noise_busy NOTIFY aiChanged)
    Q_PROPERTY(QString foundationAiDenoiseNoiseLevel MEMBER noise_level NOTIFY aiChanged)
    Q_PROPERTY(int foundationAiDenoiseNoiseScore MEMBER noise_score NOTIFY aiChanged)
    Q_PROPERTY(int foundationAiDenoiseNoiseConfidence MEMBER noise_confidence NOTIFY aiChanged)
    Q_PROPERTY(
        QString foundationAiDenoiseNoiseRecommendation MEMBER noise_recommendation NOTIFY aiChanged
    )
    Q_PROPERTY(
        int foundationWhiteBalanceTemperature MEMBER foundation_temperature NOTIFY valuesChanged
    )
    Q_PROPERTY(int foundationWhiteBalanceTint MEMBER foundation_tint NOTIFY valuesChanged)
    Q_PROPERTY(
        bool foundationWhiteBalanceAtCameraValue MEMBER foundation_at_camera NOTIFY valuesChanged
    )
    Q_PROPERTY(
        bool foundationWhiteBalanceCameraValueAvailable MEMBER foundation_camera_value_available
            NOTIFY valuesChanged
    )
    Q_PROPERTY(bool foundationSelected MEMBER foundation_selected NOTIFY valuesChanged)
    Q_PROPERTY(
        bool whiteBalancePickerActive MEMBER white_balance_picker_active NOTIFY valuesChanged
    )
    Q_PROPERTY(double whiteBalanceTemperature MEMBER white_balance_temperature NOTIFY valuesChanged)
    Q_PROPERTY(double whiteBalanceTint MEMBER white_balance_tint NOTIFY valuesChanged)
    Q_PROPERTY(double exposureStops MEMBER exposure NOTIFY valuesChanged)
    Q_PROPERTY(double contrastFactor MEMBER contrast NOTIFY valuesChanged)
    Q_PROPERTY(double saturationFactor MEMBER saturation NOTIFY valuesChanged)
    Q_PROPERTY(quint64 parameterRevision MEMBER parameter_revision NOTIFY valuesChanged)

  public:
    [[nodiscard]] bool foundationAiDenoiseEnabled() const noexcept {
        return ai_enabled;
    }

    void setFoundationAiDenoiseEnabled(const bool enabled) {
        ++toggle_count;
        ai_enabled = enabled;
        ai_requested = enabled;
        emit aiChanged();
    }

    [[nodiscard]] int foundationAiDenoiseAmount() const noexcept {
        return ai_amount;
    }

    void setFoundationAiDenoiseAmount(const int amount) {
        ai_amount = amount;
        emit aiChanged();
    }

    Q_INVOKABLE void startFoundationAiDenoise() {
        ++start_count;
        ai_requested = true;
        emit aiChanged();
    }

    Q_INVOKABLE void cancelFoundationAiDenoise() {
        ++cancel_count;
        ai_requested = false;
        emit aiChanged();
    }

    Q_INVOKABLE double parameterValue(const QString&) const {
        return 0.0;
    }

    Q_INVOKABLE void beginParameterEdit(const QString&) {}
    Q_INVOKABLE void endParameterEdit(const QString&) {}
    Q_INVOKABLE void setParameterValue(const QString&, double) {}
    Q_INVOKABLE void setWhiteBalancePickerActive(bool) {}
    Q_INVOKABLE void resetFoundationWhiteBalance() {
        ++white_balance_reset_count;
        foundation_at_camera = true;
        emit valuesChanged();
    }

    void setAiState(
        const bool enabled,
        const bool busy,
        const bool can_start,
        const bool can_apply,
        const bool can_cancel,
        QString phase,
        const double progress,
        QString status
    ) {
        ai_enabled = enabled;
        ai_requested = enabled || busy;
        ai_busy = busy;
        ai_can_start = can_start;
        ai_can_apply = can_apply;
        ai_can_cancel = can_cancel;
        ai_phase = std::move(phase);
        ai_progress = progress;
        ai_status = std::move(status);
        emit aiChanged();
    }

    bool active = true;
    bool state_busy = false;
    bool ai_enabled = false;
    bool ai_requested = false;
    bool raw_denoise_node_visible = true;
    int ai_amount = 100;
    bool ai_available = true;
    bool ai_busy = false;
    bool ai_can_start = true;
    bool ai_can_apply = false;
    bool ai_can_cancel = false;
    QString ai_phase = QStringLiteral("available");
    double ai_progress = 0.0;
    QString ai_status = QStringLiteral("available");
    bool noise_busy = false;
    QString noise_level = QStringLiteral("low");
    int noise_score = 18;
    int noise_confidence = 82;
    QString noise_recommendation = QStringLiteral("Low noise · AI denoise likely unnecessary");
    int foundation_temperature = 6'200;
    int foundation_tint = -8;
    bool foundation_at_camera = true;
    bool foundation_camera_value_available = true;
    bool foundation_selected = false;
    bool white_balance_picker_active = false;
    double white_balance_temperature = 0.0;
    double white_balance_tint = 0.0;
    double exposure = 0.0;
    double contrast = 1.0;
    double saturation = 1.0;
    quint64 parameter_revision = 0;
    int toggle_count = 0;
    int start_count = 0;
    int cancel_count = 0;
    int white_balance_reset_count = 0;

  signals:
    void aiChanged();
    void valuesChanged();
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision RAW Foundation contract failed: " << message << '\n';
    }
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent foundation_component{&engine};
    foundation_component.loadFromModule(
        QStringLiteral("Shadow.RawFoundationAdjustmentsContract"),
        QStringLiteral("PrecisionFoundationAdjustments")
    );

    FoundationEditorStub editor;
    std::unique_ptr<QObject> foundation_object{foundation_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("gradeControlsEnabled"), true},
        {QStringLiteral("panelRaised"), QColor{QStringLiteral("#20252b")}},
        {QStringLiteral("panelBorder"), QColor{QStringLiteral("#3a424b")}},
        {QStringLiteral("textPrimary"), QColor{QStringLiteral("#f2f4f6")}},
        {QStringLiteral("textMuted"), QColor{QStringLiteral("#9ca6af")}},
        {QStringLiteral("accent"), QColor{QStringLiteral("#65a8e8")}},
        {QStringLiteral("width"), 320.0},
    })};
    auto* const foundation_root = qobject_cast<QQuickItem*>(foundation_object.get());
    if (!foundation_root) {
        std::cerr << foundation_component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQmlComponent denoise_component{&engine};
    denoise_component.loadFromModule(
        QStringLiteral("Shadow.RawFoundationAdjustmentsContract"),
        QStringLiteral("PrecisionRawDenoiseAdjustments")
    );
    std::unique_ptr<QObject> denoise_object{denoise_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("textPrimary"), QColor{QStringLiteral("#f2f4f6")}},
        {QStringLiteral("textMuted"), QColor{QStringLiteral("#9ca6af")}},
        {QStringLiteral("accent"), QColor{QStringLiteral("#65a8e8")}},
        {QStringLiteral("width"), 320.0},
    })};
    auto* const denoise_root = qobject_cast<QQuickItem*>(denoise_object.get());
    if (!denoise_root) {
        std::cerr << denoise_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    drainBindings();

    auto* const amount =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseAmountSlider"));
    auto* const progress =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseProgress"));
    auto* const enable =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseEnableCheckBox"));
    auto* const noise_recommendation =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseNoiseRecommendation"));
    auto* const hidden_warning =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseNodeHiddenWarning"));
    auto* const cancel =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseCancelButton"));
    auto* const bypass =
        denoise_root->findChild<QQuickItem*>(QStringLiteral("rawAiDenoiseBypassButton"));
    auto* const temperature = foundation_root->findChild<QQuickItem*>(
        QStringLiteral("foundationWhiteBalanceTemperatureSlider")
    );
    auto* const tint =
        foundation_root->findChild<QQuickItem*>(QStringLiteral("foundationWhiteBalanceTintSlider"));
    auto* const white_balance_reset = foundation_root->findChild<QQuickItem*>(
        QStringLiteral("foundationWhiteBalanceResetButton")
    );
    if (!require(amount != nullptr, "cached-result amount control is packaged")
        || !require(progress != nullptr, "progress surface is packaged")
        || !require(enable != nullptr, "explicit enable checkbox is packaged")
        || !require(noise_recommendation != nullptr, "source noise advice is packaged")
        || !require(hidden_warning != nullptr, "hidden-node truth is packaged")
        || !require(cancel != nullptr, "cancel action is packaged")
        || !require(bypass != nullptr, "header bypass action is packaged")
        || !require(temperature != nullptr, "absolute Kelvin control is packaged")
        || !require(tint != nullptr, "absolute tint control is packaged")
        || !require(white_balance_reset != nullptr, "camera-value reset is packaged")
        || !require(
            std::abs(temperature->property("value").toDouble() - 6'200.0) < 0.0001,
            "camera white balance is presented as Kelvin"
        )
        || !require(
            std::abs(tint->property("value").toDouble() + 8.0) < 0.0001,
            "camera tint is presented on the photographic tint axis"
        )
        || !require(
            !white_balance_reset->property("enabled").toBool(),
            "camera-derived value does not expose a redundant mode action"
        )
        || !require(
            std::abs(amount->property("value").toDouble() - 100.0) < 0.0001,
            "AI denoise starts at full cached-result amount"
        )
        || !require(
            amount->property("fillFromMinimum").toBool(),
            "AI denoise amount fills from zero to its current value"
        )
        || !require(!amount->property("enabled").toBool(), "bypassed amount is read-only")
        || !require(!bypass->property("enabled").toBool(), "bypassed node disables reset")
        || !require(
            enable->property("visible").toBool() && enable->property("enabled").toBool()
                && !enable->property("checked").toBool(),
            "available state exposes an unchecked enable choice"
        )
        || !require(!progress->property("visible").toBool(), "idle state hides progress")) {
        return EXIT_FAILURE;
    }
    if (!require(
            !hidden_warning->property("visible").toBool(),
            "a visible node does not show a contradictory warning"
        )) {
        return EXIT_FAILURE;
    }
    if (!require(
            noise_recommendation->property("text").toString()
                == QStringLiteral("Low noise · AI denoise likely unnecessary"),
            "panel presents the confidence-aware source recommendation"
        )) {
        return EXIT_FAILURE;
    }

    QMetaObject::invokeMethod(enable, "clicked");
    drainBindings();
    if (!require(
            editor.start_count == 1 && editor.toggle_count == 0
                && !editor.foundationAiDenoiseEnabled(),
            "checking the option starts model work without prematurely enabling the Recipe"
        )) {
        return EXIT_FAILURE;
    }

    editor.setAiState(
        false,
        true,
        false,
        false,
        true,
        QStringLiteral("running"),
        0.05,
        QStringLiteral("running")
    );
    drainBindings();
    if (!require(progress->property("visible").toBool(), "busy state exposes progress")
        || !require(
            std::abs(progress->property("value").toDouble() - 0.05) < 0.0001
                && progress->property("indeterminate").toBool(),
            "the provider's long 5% execution phase is presented as indeterminate"
        )
        || !require(cancel->property("visible").toBool(), "busy state exposes cancel")
        || !require(
            cancel->property("source").toUrl() == QUrl{QStringLiteral("qrc:/icons/close.svg")}
                && cancel->property("buttonSize").toInt() == 22,
            "busy cancellation is a compact close action beside progress"
        )
        || !require(
            enable->property("checked").toBool() && enable->property("enabled").toBool(),
            "busy state keeps the requested option checked and cancellable"
        )) {
        return EXIT_FAILURE;
    }
    QMetaObject::invokeMethod(cancel, "clicked");
    if (!require(editor.cancel_count == 1, "cancel delegates exactly once")) {
        return EXIT_FAILURE;
    }

    editor.setAiState(
        true,
        false,
        false,
        false,
        false,
        QStringLiteral("ready"),
        1.0,
        QStringLiteral("enabled")
    );
    drainBindings();
    if (!require(amount->property("enabled").toBool(), "Ready Recipe enables fast amount changes")
        || !require(!progress->property("visible").toBool(), "Ready state hides progress")
        || !require(enable->property("checked").toBool(), "Ready state keeps enable checked")
        || !require(bypass->property("enabled").toBool(), "enabled node exposes bypass")) {
        return EXIT_FAILURE;
    }

    editor.raw_denoise_node_visible = false;
    emit editor.aiChanged();
    drainBindings();
    if (!require(
            hidden_warning->property("visible").toBool()
                && hidden_warning->property("text").toString()
                       == QStringLiteral("Node hidden · AI result is not applied"),
            "enabled AI intent states clearly when node visibility prevents application"
        )) {
        return EXIT_FAILURE;
    }
    editor.raw_denoise_node_visible = true;
    emit editor.aiChanged();
    drainBindings();

    QMetaObject::invokeMethod(bypass, "clicked");
    drainBindings();
    if (!require(
            editor.toggle_count == 1 && !editor.foundationAiDenoiseEnabled()
                && !enable->property("checked").toBool(),
            "header bypass delegates one non-destructive Recipe transition"
        )) {
        return EXIT_FAILURE;
    }
    editor.foundation_at_camera = false;
    emit editor.valuesChanged();
    drainBindings();
    if (!require(
            white_balance_reset->property("enabled").toBool(),
            "an authored absolute value exposes camera-value reset"
        )) {
        return EXIT_FAILURE;
    }
    QMetaObject::invokeMethod(white_balance_reset, "clicked");
    drainBindings();
    return require(
               editor.white_balance_reset_count == 1 && editor.foundation_at_camera,
               "section-header reset restores the camera-derived absolute value"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "precision_raw_foundation_adjustments_contract_test.moc"
