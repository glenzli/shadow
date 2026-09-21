#include "rgb_curve_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrl>
#include <memory>

void installRgbCurveSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    struct State {
        int stage = 0;
        BackendGradeStack saved;
        QElapsedTimer lifetime, gesture;
    };
    auto state = std::make_shared<State>();
    state->lifetime.start();
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, state, timer] {
        const auto fail = [&](const char* reason) {
            qCritical() << "RGB curve acceptance failed" << state->stage << reason
                        << editor.statusText() << pipeline.errorText();
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->lifetime.elapsed() > 240000) {
            fail("timeout");
            return;
        }
        if (pipeline.finished()) {
            qInfo() << "RGB curve acceptance completed: five curves, gesture undo/redo, reset, "
                       "photo isolation, persistence, export";
            timer->stop();
            pipeline.cancel();
            return;
        }
        if (state->stage == 5 && !pipeline.busy() && !pipeline.errorText().isEmpty()) {
            fail("export");
            return;
        }
        if (pipeline.busy() || editor.busy() || !editor.active())
            return;
        auto* workspace =
            engine.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (!workspace || !workspace->property("previewFrameReady").toBool())
            return;
        switch (state->stage) {
        case 0:
            if (pipeline.photoCount() != 2) {
                fail("requires two local fixtures");
                return;
            }
            editor.selectGradeNode(0);
            for (int channel = 0; channel < 5; ++channel) {
                editor.setToneCurveChannel(channel);
                editor.addToneCurvePoint(0.5, 0.53 + channel * 0.01);
            }
            state->stage = 1;
            return;
        case 1: {
            const auto before = editor.gradeStackForInterchange();
            for (const auto& modified : editor.toneCurveModifiedChannels())
                if (!modified.toBool()) {
                    fail("missing authored channel");
                    return;
                }
            state->gesture.start();
            editor.beginToneCurveGesture(1);
            for (int i = 0; i < 96; ++i)
                editor.moveToneCurvePoint(1, 0.5, 0.52 + (i % 20) * 0.002);
            editor.endToneCurveGesture(1);
            const auto after = editor.gradeStackForInterchange();
            editor.undo();
            if (editor.gradeStackForInterchange() != before) {
                fail("gesture undo");
                return;
            }
            editor.redo();
            if (editor.gradeStackForInterchange() != after) {
                fail("gesture redo");
                return;
            }
            editor.resetToneCurve();
            if (editor.hasToneCurve() || !editor.toneCurveModifiedChannels()[2].toBool()) {
                fail("channel-only reset");
                return;
            }
            editor.undo();
            if (editor.gradeStackForInterchange() != after) {
                fail("reset undo");
                return;
            }
            editor.resetAllToneCurves();
            for (const auto& modified : editor.toneCurveModifiedChannels())
                if (modified.toBool()) {
                    fail("all-channel reset");
                    return;
                }
            editor.undo();
            if (editor.gradeStackForInterchange() != after) {
                fail("all-channel reset undo");
                return;
            }
            state->saved = after;
            state->stage = 2;
            return;
        }
        case 2:
            qInfo() << "RGB curve 96-event burst plus history settled ms"
                    << state->gesture.elapsed();
            state->stage = 3;
            pipeline.selectPhoto(1);
            return;
        case 3:
            if (pipeline.currentIndex() != 1)
                return;
            for (const auto& modified : editor.toneCurveModifiedChannels())
                if (modified.toBool()) {
                    fail("cross-photo contamination");
                    return;
                }
            state->stage = 4;
            pipeline.selectPhoto(0);
            return;
        case 4:
            if (pipeline.currentIndex() != 0)
                return;
            if (editor.gradeStackForInterchange() != state->saved) {
                fail("saved curves did not reopen exactly");
                return;
            }
            if (!pipeline.configureExport(
                    QUrl::fromLocalFile(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT")),
                    {{QStringLiteral("format"), QStringLiteral("png")}}
                )) {
                fail("configure export");
                return;
            }
            state->stage = 5;
            pipeline.complete();
            return;
        default:
            return;
        }
    });
    timer->start();
}
