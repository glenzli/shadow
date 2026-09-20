#include "subject_emphasis_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_subject_emphasis_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <cmath>
#include <memory>

namespace {
struct AcceptanceState {
    int stage = 0;
    BackendGradeStack before, enhanced;
    bool had_undo = false;
    QElapsedTimer elapsed;
};
} // namespace

void installSubjectEmphasisSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    const QString query = qEnvironmentVariable("SHADOW_SUBJECT_EMPHASIS_SMOKE_QUERY");
    if (query.isEmpty())
        return;
    auto* const feature = qobject_cast<EditSubjectEmphasisController*>(editor.subjectEmphasis());
    auto* const timer = new QTimer(&pipeline);
    timer->setInterval(50);
    auto state = std::make_shared<AcceptanceState>();
    state->elapsed.start();
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, query, feature, timer, state] {
        const auto fail = [&](const QString& message) {
            qCritical().noquote() << "Subject emphasis acceptance failed at stage" << state->stage
                                  << message << "pipeline:" << pipeline.statusText()
                                  << pipeline.errorText()
                                  << "pending/exporting:" << pipeline.exportPending()
                                  << pipeline.exporting() << "editor:" << editor.statusText()
                                  << "active/busy/stateBusy/rendering/autosave:" << editor.active()
                                  << editor.busy() << editor.stateBusy() << editor.rendering()
                                  << editor.autosavePending();
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->elapsed.elapsed() > 240000) {
            fail(feature->status());
            return;
        }
        if (pipeline.finished()) {
            timer->stop();
            pipeline.cancel();
            return;
        }
        if (editor.autosaveFailed()) {
            fail(QStringLiteral("autosave failed"));
            return;
        }
        if (!editor.active() || (pipeline.busy() && !feature->active()))
            return;
        auto* const workspace =
            engine.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (!workspace || !workspace->property("previewFrameReady").toBool())
            return;
        if (feature->busy() || editor.busy() || editor.autosavePending())
            return;
        switch (state->stage) {
        case 0:
            editor.selectFoundationNode();
            state->before = editor.gradeStackForInterchange();
            state->had_undo = editor.canUndo();
            feature->analyze();
            feature->cancel();
            state->stage = 1;
            return;
        case 1:
            if (feature->active() || editor.gradeStackForInterchange() != state->before
                || editor.canUndo() != state->had_undo) {
                fail(QStringLiteral("cancel changed the Recipe or history"));
                return;
            }
            feature->analyze();
            state->stage = 2;
            return;
        case 2:
            if (!feature->analyzed() || editor.gradeStackForInterchange() != state->before) {
                fail(feature->status());
                return;
            }
            feature->selectSubject(query);
            state->stage = 3;
            return;
        case 3: {
            auto* const overlay = workspace->findChild<QObject*>("subjectEmphasisCandidateOverlay");
            if (!feature->canApply() || !overlay || !overlay->property("visible").toBool()
                || editor.gradeStackForInterchange() != state->before) {
                fail(feature->status());
                return;
            }
            feature->apply();
            state->stage = 4;
            return;
        }
        case 4:
            state->enhanced = editor.gradeStackForInterchange();
            if (editor.selectedRecipeNodeKind() != QStringLiteral("grade")
                || state->enhanced.grade_nodes.size() != state->before.grade_nodes.size() + 1
                || state->enhanced.grade_nodes.last().local_mask_components.size() != 1
                || std::abs(state->enhanced.grade_nodes.last().basic.exposure_stops) > 0.2) {
                fail(QStringLiteral("apply did not create one restrained masked node"));
                return;
            }
            editor.undo();
            state->stage = 5;
            return;
        case 5:
            if (editor.gradeStackForInterchange() != state->before || feature->applied()
                || !feature->status().isEmpty()) {
                fail(QStringLiteral("one undo did not restore the original draft"));
                return;
            }
            editor.redo();
            state->stage = 6;
            return;
        case 6:
            if (editor.gradeStackForInterchange() != state->enhanced || !feature->applied()) {
                const auto current = editor.gradeStackForInterchange();
                qWarning() << "Redo comparison: applied/kind/id/nodes/foundation/raw/geometry"
                           << feature->applied() << editor.selectedRecipeNodeKind()
                           << editor.selectedGradeNodeId()
                           << (current.grade_nodes == state->enhanced.grade_nodes)
                           << (current.foundation == state->enhanced.foundation)
                           << (current.raw_ai_denoise == state->enhanced.raw_ai_denoise)
                           << (current.geometry == state->enhanced.geometry);
                fail(QStringLiteral("redo did not restore the same node and mask"));
                return;
            }
            editor.beginParameterEdit(QStringLiteral("node/strength"));
            feature->setStrength(0.4);
            editor.endParameterEdit(QStringLiteral("node/strength"));
            state->stage = 7;
            return;
        case 7:
            state->enhanced.grade_nodes.last().opacity = 0.4;
            if (editor.gradeStackForInterchange() != state->enhanced || feature->busy()) {
                fail(QStringLiteral("strength changed more than downstream node opacity"));
                return;
            }
            qInfo() << "Subject emphasis acceptance passed: cancel, local Qwen, grounding/SAM, "
                       "canvas overlay, apply, undo, redo, strength"
                    << state->elapsed.elapsed() << "ms";
            state->stage = 8;
            if (pipeline.interactive()
                && !pipeline.configureExport(
                    QUrl::fromLocalFile(
                        qEnvironmentVariable("SHADOW_SUBJECT_EMPHASIS_SMOKE_OUTPUT")
                    ),
                    {{QStringLiteral("format"), QStringLiteral("jpeg")}}
                )) {
                fail(QStringLiteral("export destination unavailable"));
                return;
            }
            pipeline.complete();
            qInfo() << "Subject emphasis export requested:" << pipeline.exportPending()
                    << pipeline.statusText() << pipeline.errorText() << editor.statusText();
            return;
        default:
            return;
        }
    });
    timer->start();
}
