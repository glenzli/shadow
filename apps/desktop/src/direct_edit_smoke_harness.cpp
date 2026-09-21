#include "direct_edit_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"
#include "edit_targeted_curve_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrl>
#include <memory>

void installDirectEditSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    struct State {
        int stage = 0, channel = 0, retries = 0;
        QElapsedTimer lifetime, sampling;
        BackendGradeStack saved;
    };
    auto state = std::make_shared<State>();
    state->lifetime.start();
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, state, timer] {
        const auto fail = [&](const char* reason) {
            qCritical() << "Direct edit acceptance failed" << state->stage << state->channel
                        << reason;
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->lifetime.elapsed() > 600000) {
            fail("timeout");
            return;
        }
        if (pipeline.finished()) {
            qInfo() << "Direct edit acceptance completed: five sampled channels, cancellation, "
                       "history, Dodge/Burn, isolation, persistence, export";
            timer->stop();
            pipeline.cancel();
            return;
        }
        if (pipeline.busy() || editor.busy() || !editor.active())
            return;
        auto* workspace =
            engine.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (!workspace || !workspace->property("previewFrameReady").toBool())
            return;
        auto* tool = qobject_cast<EditTargetedCurveController*>(editor.targetedCurve());
        auto* paint = qobject_cast<EditPaintController*>(editor.paint());
        if (!tool || !paint) {
            fail("missing controllers");
            return;
        }
        switch (state->stage) {
        case 0:
            if (pipeline.photoCount() != 2) {
                fail("requires two fixtures");
                return;
            }
            editor.selectGradeNode(0);
            editor.setToneCurveChannel(0);
            tool->setActive(true);
            state->sampling.start();
            state->stage = 1;
            return;
        case 1: {
            if (!tool->ready()) {
                if (!tool->busy() && ++state->retries < 4)
                    tool->refresh();
                if (state->retries >= 4)
                    fail("input map unavailable");
                return;
            }
            qInfo() << "Curve input preparation ms" << state->channel << state->sampling.elapsed();
            QElapsedTimer hover;
            hover.start();
            for (int i = 0; i < 1000; ++i)
                tool->hover(0.4 + (i % 10) * 0.01, 0.5);
            qInfo() << "1000 cached hover samples ms" << hover.elapsed();
            const auto before = editor.gradeStackForInterchange();
            if (!tool->begin(0.5, 0.5)) {
                fail("gesture begin");
                return;
            }
            for (int i = 1; i <= 60; ++i)
                tool->move(-0.001 * i);
            tool->finish(true);
            if (editor.gradeStackForInterchange() != before) {
                fail("cancel changed recipe");
                return;
            }
            if (!tool->begin(0.5, 0.5)) {
                fail("repeat begin");
                return;
            }
            tool->move(-0.04);
            tool->finish();
            const auto after = editor.gradeStackForInterchange();
            if (after == before) {
                fail("drag had no effect");
                return;
            }
            editor.undo();
            if (editor.gradeStackForInterchange() != before) {
                fail("single gesture undo");
                return;
            }
            editor.redo();
            if (editor.gradeStackForInterchange() != after) {
                fail("exact redo");
                return;
            }
            if (++state->channel < 5) {
                editor.setToneCurveChannel(state->channel);
                state->retries = 0;
                state->sampling.restart();
                return;
            }
            tool->setActive(false);
            state->stage = 2;
            return;
        }
        case 2: {
            paint->activate();
            const auto color = paint->color();
            const auto flow = paint->flow();
            paint->setDodgeBurn(true);
            const auto before = editor.gradeStackForInterchange();
            if (paint->flow() != 0.05 || !paint->beginStroke(0.48, 0.5, 1.5, 0.6)) {
                fail("Dodge admission");
                return;
            }
            paint->appendPoint(0.52, 0.5, 0.8);
            paint->finishStroke();
            const auto after = editor.gradeStackForInterchange();
            editor.undo();
            if (editor.gradeStackForInterchange() != before) {
                fail("Dodge undo");
                return;
            }
            editor.redo();
            if (editor.gradeStackForInterchange() != after) {
                fail("Dodge redo");
                return;
            }
            if (!paint->beginStroke(0.4, 0.5, 1.5, 1, true)) {
                fail("reverse admission");
                return;
            }
            paint->finishStroke();
            const auto layers = editor.gradeStackForInterchange().paint_layers;
            if (layers.size() != 1 || layers[0].blend != 2 || layers[0].strokes.size() != 2
                || layers[0].strokes[0].red != 1 || layers[0].strokes[1].red != 0
                || paint->burn()) {
                fail("reverse changed brush mode or blend");
                return;
            }
            const auto complete = editor.gradeStackForInterchange();
            paint->setBurn(true);
            paint->beginStroke(0.5, 0.4, 1.5);
            paint->cancelStroke();
            if (editor.gradeStackForInterchange() != complete) {
                fail("Burn cancellation");
                return;
            }
            paint->setLayerOpacity(0.7);
            paint->setDodgeBurn(false);
            if (paint->color() != color || paint->flow() != flow) {
                fail("general brush preferences changed");
                return;
            }
            state->saved = editor.gradeStackForInterchange();
            state->stage = 3;
            pipeline.selectPhoto(1);
            return;
        }
        case 3:
            if (pipeline.currentIndex() != 1)
                return;
            if (tool->active() || !editor.gradeStackForInterchange().paint_layers.isEmpty()) {
                fail("photo isolation");
                return;
            }
            state->stage = 4;
            pipeline.selectPhoto(0);
            return;
        case 4:
            if (pipeline.currentIndex() != 0)
                return;
            if (editor.gradeStackForInterchange() != state->saved) {
                fail("persisted recipe mismatch");
                return;
            }
            if (!pipeline.configureExport(
                    QUrl::fromLocalFile(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT")),
                    {{QStringLiteral("format"),
                      qEnvironmentVariable("SHADOW_DIRECT_EDIT_EXPORT_FORMAT", "png")}}
                )) {
                fail("export setup");
                return;
            }
            state->stage = 5;
            pipeline.complete();
            return;
        default:
            if (!pipeline.errorText().isEmpty())
                fail("export");
        }
    });
    timer->start();
}
