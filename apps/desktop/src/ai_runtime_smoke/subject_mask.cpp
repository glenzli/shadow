#include "../edit_controller.hpp"
#include "session.hpp"
#include <QDebug>

namespace AiRuntimeSmoke {
namespace {
class SubjectMask final : public Session {
  public:
    SubjectMask(
        QQmlApplicationEngine& engine,
        PipelineRunController& pipeline,
        EditController& editor,
        QString mode
    ) : Session(engine, pipeline, editor, mode), mode_(std::move(mode)) {}

  private:
    void open() {
        checkpoint("opening selection");
        const bool opened =
            mode_ == QStringLiteral("ai-semantic")
                ? editor_.beginAiSemanticMaskForOperation(QStringLiteral("person"), 0, true)
            : mode_ == QStringLiteral("ai-face")
                ? editor_.beginAiFaceMaskPromptForOperation(0, true)
                : editor_.beginAiMaskPromptForOperation(0, true);
        require(opened, "AI selection could not open on the selected Grade Node");
    }

    void prompt() {
        if (mode_.startsWith(QStringLiteral("ai-subject"))) {
            click("aiMaskPromptSurface", {.55, .55});
            checkpoint("subject point delivered");
            require(
                editor_.aiMaskPromptPoints().size() == 1,
                "canvas did not receive the subject point"
            );
            require(editor_.aiMaskBusy(), "the canvas point did not admit an AI request");
        }
    }

    bool candidate() {
        if (editor_.aiMaskBusy())
            return false;
        if (mode_ == QStringLiteral("ai-subject-unavailable")) {
            require(
                !editor_.aiMaskHasCandidate() && editor_.gradeStackForInterchange() == before_,
                "unavailable inference published a selection or changed the Recipe"
            );
            return true;
        }
        if (mode_ == QStringLiteral("ai-face") && editor_.aiMaskSelectedPerson() < 0) {
            require(
                !editor_.aiMaskPeople().isEmpty(),
                "real face parser did not discover the portrait"
            );
            editor_.setAiMaskSelectedPerson(0);
            return false;
        }
        require(
            editor_.aiMaskHasCandidate() && !editor_.aiMaskCandidateSource().isEmpty(),
            "real model did not publish a selection candidate"
        );
        require(
            editor_.gradeStackForInterchange() == before_,
            "transient AI selection mutated the Recipe"
        );
        return true;
    }

    void advance() override {
        switch (stage_) {
        case 0:
            checkpoint("preparing Grade Node");
            editor_.addGradeNode();
            editor_.setExposureStops(.35);
            stage_ = 1;
            return;
        case 1:
            checkpoint("capturing stable edit");
            before_ = editor_.gradeStackForInterchange();
            undo_before_ = editor_.canUndo();
            open();
            stage_ = 2;
            return;
        case 2:
            prompt();
            stage_ = 3;
            return;
        case 3:
            if (!candidate())
                return;
            click("aiMaskCancelButton");
            stage_ = 4;
            return;
        case 4:
            require(
                !editor_.aiMaskPromptActive() && !editor_.aiMaskHasCandidate()
                    && editor_.gradeStackForInterchange() == before_
                    && editor_.canUndo() == undo_before_,
                "cancel did not retire the disposable selection"
            );
            qInfo().noquote() << "AI editor" << mode_ << "candidate/cancel passed";
            if (mode_ == QStringLiteral("ai-subject-unavailable")) {
                checkpoint("unavailable/cancel passed");
                exportNow();
                return;
            }
            open();
            stage_ = 5;
            return;
        case 5:
            prompt();
            stage_ = 6;
            return;
        case 6:
            if (!candidate())
                return;
            if (mode_ == QStringLiteral("ai-subject")) {
                click("aiMaskSubtractSelectionButton");
                click("aiMaskPromptSurface", {.12, .15});
                require(
                    editor_.aiMaskPromptPoints().size() == 2,
                    "refinement did not retain the include and exclude points"
                );
            } else if (mode_ == QStringLiteral("ai-face")) {
                editor_.setAiMaskFaceRegion(static_cast<int>(BackendFaceRegion::Skin));
            }
            stage_ = 7;
            return;
        case 7:
            if (!candidate())
                return;
            qInfo().noquote() << "AI editor" << mode_ << "current candidate passed";
            click("aiMaskApplyButton");
            stage_ = 8;
            return;
        case 8: {
            require(!editor_.aiMaskPromptActive(), "AI apply did not finish");
            const auto stack = editor_.gradeStackForInterchange();
            require(
                stack.grade_nodes.size() == before_.grade_nodes.size()
                    && !stack.grade_nodes.back().local_mask_components.isEmpty(),
                "accepted selection did not become a Grade Node mask"
            );
            stage_ = 9;
            acceptEdit(before_);
            return;
        }
        default:
            require(false, "unexpected selection acceptance stage");
        }
    }

    QString mode_;
    BackendGradeStack before_;
    int stage_ = 0;
    bool undo_before_ = false;
};
} // namespace

void installSubjectMask(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor,
    QString mode
) {
    new SubjectMask(engine, pipeline, editor, std::move(mode));
}
} // namespace AiRuntimeSmoke
