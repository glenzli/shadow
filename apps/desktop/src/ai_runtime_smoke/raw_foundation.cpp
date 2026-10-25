#include "../edit_controller.hpp"
#include "session.hpp"
#include <QDebug>

namespace AiRuntimeSmoke {
namespace {
class RawFoundation final : public Session {
  public:
    RawFoundation(
        QQmlApplicationEngine& engine,
        PipelineRunController& pipeline,
        EditController& editor
    ) : Session(engine, pipeline, editor, QStringLiteral("ai-raw-foundation")) {}

  private:
    void observe() override {
        if (stage_ >= 4)
            require(
                !editor_.foundationAiDenoiseBusy(),
                "an interactive edit restarted RAW materialization or discovery"
            );
    }

    void historyVerified() override {
        require(
            editor_.foundationAiDenoiseEnabled(),
            "redo did not enable the cached RAW foundation"
        );
        require(
            editor_.foundationWhiteBalanceCameraValueAvailable(),
            "AI RAW changed camera white balance availability"
        );
        editor_.selectRawDenoiseNode();
        preview_ = editor_.previewSource();
        stage_ = 4;
    }

    void advance() override {
        switch (stage_) {
        case 0:
            require(
                editor_.foundationWhiteBalanceCameraValueAvailable(),
                "AI RAW acceptance requires a calibrated RAW source"
            );
            editor_.addRawDenoiseNode();
            stage_ = 1;
            return;
        case 1:
            if (editor_.foundationAiDenoiseBusy())
                return;
            require(
                editor_.rawDenoiseNodeMaterialized() && editor_.foundationAiDenoiseAvailable(),
                "real local RAW denoise model is unavailable"
            );
            before_ = editor_.gradeStackForInterchange();
            editor_.startFoundationAiDenoise();
            stage_ = 2;
            return;
        case 2:
            if (editor_.foundationAiDenoiseBusy())
                return;
            require(
                editor_.foundationAiDenoiseEnabled()
                    && editor_.foundationAiDenoisePhase() == QStringLiteral("ready"),
                "real RAW model did not materialize and apply its verified foundation"
            );
            qInfo() << "AI editor ai-raw-foundation materialized passed";
            stage_ = 3;
            acceptEdit(before_);
            return;
        case 4:
            slide("rawAiDenoiseAmountSlider", .65);
            amount_ = editor_.foundationAiDenoiseAmount();
            require(
                amount_ > 0 && amount_ < 100,
                "RAW strength did not receive real pointer input"
            );
            stage_ = 5;
            return;
        case 5:
            require(
                !editor_.foundationAiDenoiseBusy() && editor_.foundationAiDenoiseEnabled()
                    && editor_.foundationAiDenoiseAmount() == amount_,
                "amount edit did not reuse the ready RAW foundation"
            );
            if (editor_.previewSource() == preview_)
                return;
            require(
                editor_.foundationWhiteBalanceCameraValueAvailable(),
                "AI RAW hid the calibrated white balance controls"
            );
            preview_ = editor_.previewSource();
            temperature_ = editor_.foundationWhiteBalanceTemperature();
            editor_.selectFoundationNode();
            stage_ = 6;
            return;
        case 6:
            slide("foundationWhiteBalanceTemperatureSlider", .30);
            require(
                editor_.foundationWhiteBalanceTemperature() != temperature_,
                "RAW white balance did not receive real pointer input"
            );
            temperature_ = editor_.foundationWhiteBalanceTemperature();
            stage_ = 7;
            return;
        case 7:
            require(
                !editor_.foundationAiDenoiseBusy() && editor_.foundationAiDenoiseEnabled(),
                "white balance invalidated the materialized RAW foundation"
            );
            if (editor_.previewSource() == preview_)
                return;
            require(
                editor_.foundationWhiteBalanceCameraValueAvailable()
                    && editor_.foundationWhiteBalanceTemperature() == temperature_,
                "AI RAW lost the authored white point"
            );
            qInfo() << "AI editor ai-raw-foundation amount/white-balance pointer preview passed"
                    << amount_ << temperature_;
            exportNow();
            return;
        default:
            require(false, "unexpected RAW foundation acceptance stage");
        }
    }

    BackendGradeStack before_;
    QString preview_;
    int stage_ = 0;
    int amount_ = 0, temperature_ = 0;
};
} // namespace

void installRawFoundation(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    new RawFoundation(engine, pipeline, editor);
}
} // namespace AiRuntimeSmoke
