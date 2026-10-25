#include "../edit_controller.hpp"
#include "session.hpp"
#include <QDebug>

namespace AiRuntimeSmoke {
namespace {
class Completion final : public Session {
  public:
    Completion(
        QQmlApplicationEngine& engine,
        PipelineRunController& pipeline,
        EditController& editor
    ) : Session(engine, pipeline, editor, QStringLiteral("ai-completion")) {}

  private:
    void select() {
        click("imageCompletionToolButton");
        require(editor_.imageCompletionActive(), "AI Completion could not begin from its toolbar");
        editor_.setImageCompletionBrushRadius(.035);
        const auto stroke = editor_.beginImageCompletionStroke();
        require(stroke != 0, "AI Completion could not begin a brush stroke");
        editor_.addImageCompletionBrushPoint(.45, .72, stroke);
        editor_.addImageCompletionBrushPoint(.49, .72, stroke);
        require(
            editor_.imageCompletionBrushPoints().size() == 2
                && editor_.imageCompletionCanGenerate(),
            "AI Completion did not retain the painted selection"
        );
        editor_.generateImageCompletion();
    }

    void requireCandidate() {
        require(
            editor_.imageCompletionHasCandidate()
                && !editor_.imageCompletionCandidateSource().isEmpty(),
            "real completion model returned no candidate"
        );
        require(
            editor_.gradeStackForInterchange() == before_,
            "completion candidate mutated the Recipe before Apply"
        );
    }

    void advance() override {
        switch (stage_) {
        case 0:
            before_ = editor_.gradeStackForInterchange();
            select();
            stage_ = 1;
            return;
        case 1:
            if (editor_.imageCompletionBusy())
                return;
            requireCandidate();
            click("imageCompletionCancelButton");
            stage_ = 2;
            return;
        case 2:
            require(
                !editor_.imageCompletionActive() && !editor_.imageCompletionHasCandidate()
                    && editor_.gradeStackForInterchange() == before_,
                "cancel retained the transient completion"
            );
            qInfo() << "AI editor ai-completion candidate/cancel passed";
            select();
            stage_ = 3;
            return;
        case 3:
            if (editor_.imageCompletionBusy())
                return;
            requireCandidate();
            click("imageCompletionApplyButton");
            stage_ = 4;
            return;
        case 4: {
            if (editor_.imageCompletionBusy())
                return;
            const auto stack = editor_.gradeStackForInterchange();
            require(
                stack.image_completions.size() == before_.image_completions.size() + 1,
                "completion Apply did not publish one managed region"
            );
            const auto& region = stack.image_completions.back();
            require(
                !region.store_object_id.isEmpty() && !region.content_blake3.isEmpty()
                    && region.byte_len > 0
                    && region.actual_execution_provider
                           .contains(QStringLiteral("CUDA"), Qt::CaseInsensitive),
                "completion region lacks a verified managed raster or CUDA provenance"
            );
            qInfo().noquote() << "AI editor ai-completion CUDA provenance"
                              << region.actual_execution_provider;
            stage_ = 5;
            require(!editor_.imageCompletionActive(), "completion Apply did not release authoring");
            acceptEdit(before_);
            return;
        }
        default:
            require(false, "unexpected completion acceptance stage");
        }
    }

    BackendGradeStack before_;
    int stage_ = 0;
};
} // namespace

void installCompletion(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    new Completion(engine, pipeline, editor);
}
} // namespace AiRuntimeSmoke
