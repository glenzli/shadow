#include "paint_interaction_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <memory>

// Timed interaction acceptance deliberately overlaps a live stroke with the
// previous stroke's autosave. The ordinary paint harness waits for idle and
// therefore cannot exercise this boundary.
void installPaintInteractionSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    struct State {
        int stage = 0, samples = 0, notifications = 0, frames = 0;
        QElapsedTimer lifetime, gesture, since_frame;
        QVector<double> input_ms, frame_ms;
        QVector<BackendPaintLayer> first, second, final;
    };
    auto state = std::make_shared<State>();
    state->lifetime.start();
    auto* timer = new QTimer(&pipeline);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(16);
    auto* paint = qobject_cast<EditPaintController*>(editor.paint());
    const auto fail = [timer](const char* reason) {
        qCritical() << "Paint interaction failed:" << reason;
        timer->stop();
        QCoreApplication::exit(3);
    };
    QObject::connect(&editor, &EditController::parametersChanged, timer, [state] {
        if (state->stage == 4)
            ++state->notifications;
    });
    QObject::connect(&editor, &EditController::previewSourceChanged, timer, [state] {
        if (state->stage == 4) {
            ++state->frames;
            state->frame_ms.push_back(double(state->since_frame.restart()));
        }
    });
    // startStateTask emits this synchronously, before the save result can be
    // delivered. No sleeps, storage locks or private controller hooks are needed.
    QObject::connect(
        &editor,
        &EditController::autosavePendingChanged,
        timer,
        [&, state, paint, fail] {
            if (state->stage != 1 || !editor.busy() || editor.stateBusy() || editor.rendering()
                || editor.beforeRendering() || editor.detailRendering())
                return;
            state->stage = 2;
            if (!paint->beginStroke(0.3, 0.35, 1.5)) {
                fail("stroke rejected while prior autosave is running");
                return;
            }
            paint->appendPoint(0.4, 0.4);
            state->second = editor.gradeStackForInterchange().paint_layers;
            state->gesture.restart();
        }
    );
    QObject::connect(timer, &QTimer::timeout, timer, [&, state, paint, timer, fail] {
        const auto layers = [&] { return editor.gradeStackForInterchange().paint_layers; };
        if (state->lifetime.elapsed() > 90000) {
            fail("timeout");
            return;
        }
        if (state->stage == 2) {
            if (state->gesture.elapsed() < 1000)
                return;
            if (layers() != state->second || !paint->strokeActive()) {
                fail("older autosave replaced the live stroke");
                return;
            }
            paint->finishStroke();
            editor.undo();
            if (layers() != state->first) {
                fail("undo after overlapping autosave was not one stroke");
                return;
            }
            editor.redo();
            if (layers() != state->second) {
                fail("redo after overlapping autosave changed stroke");
                return;
            }
            if (!paint->beginStroke(0.7, 0.4, 1.5) || !editor.canUndo() || editor.canRedo()) {
                fail("live stroke history affordance is incorrect");
                return;
            }
            paint->appendPoint(0.75, 0.45);
            editor.undo();
            if (layers() != state->second || paint->strokeActive()) {
                fail("undo of live stroke also removed a preceding stroke");
                return;
            }
            state->stage = 3;
            return;
        }
        if (state->stage == 4) {
            QElapsedTimer input;
            input.start();
            const double t = double(++state->samples) / 120;
            paint->appendPoint(0.2 + t * 0.6, 0.5 + 0.12 * std::sin(t * 18));
            state->input_ms.push_back(double(input.nsecsElapsed()) / 1e6);
            if (state->samples < 120)
                return;
            const auto final = layers();
            paint->finishStroke();
            editor.undo();
            if (layers() != state->second) {
                fail("continuous stroke undo was not exact");
                return;
            }
            editor.redo();
            if (layers() != final) {
                fail("continuous stroke redo was not exact");
                return;
            }
            state->final = final;
            const auto percentile = [](QVector<double> values, double q) {
                if (values.isEmpty())
                    return -1.0;
                std::sort(values.begin(), values.end());
                return values[qsizetype(double(values.size() - 1) * q)];
            };
            qInfo() << "Paint interaction timing samples=" << state->samples
                    << "input_p50_ms=" << percentile(state->input_ms, .5)
                    << "input_p95_ms=" << percentile(state->input_ms, .95)
                    << "frames=" << state->frames
                    << "frame_p50_ms=" << percentile(state->frame_ms, .5)
                    << "frame_p95_ms=" << percentile(state->frame_ms, .95)
                    << "panel_notifications=" << state->notifications;
            if (state->frames == 0 || state->notifications > 4) {
                fail("continuous input starved preview or refreshed the full inspector per sample");
                return;
            }
            state->stage = 5;
            return;
        }
        if (pipeline.busy() || editor.busy() || !editor.active()
            || editor.previewSource().isEmpty())
            return;
        auto* workspace =
            engine.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (!workspace || !workspace->property("previewFrameReady").toBool())
            return;
        if (state->stage == 0) {
            QMetaObject::invokeMethod(workspace, "setActiveSpecialTool", Q_ARG(QVariant, 6));
            paint->setColor(QColor(220, 70, 30));
            paint->setRadius(0.035);
            paint->setFlow(0.2);
            paint->setSmoothing(0);
            if (!paint->beginStroke(0.2, 0.2, 1.5) || !editor.canUndo()) {
                fail("first live stroke cannot be undone");
                return;
            }
            editor.undo();
            if (!layers().isEmpty() || editor.canUndo()) {
                fail("first live stroke undo left ink or a phantom checkpoint");
                return;
            }
            if (!paint->beginStroke(0.3, 0.3, 1.5)) {
                fail("first stroke rejected");
                return;
            }
            paint->appendPoint(0.6, 0.3);
            paint->finishStroke();
            state->first = layers();
            state->stage = qEnvironmentVariableIsSet("SHADOW_PAINT_PERF_ONLY") ? 3 : 1;
            if (state->stage == 3)
                state->second = state->first;
        } else if (state->stage == 3) {
            if (!paint->beginStroke(0.2, 0.5, 1.5)) {
                fail("timed stroke rejected");
                return;
            }
            state->stage = 4;
            state->gesture.restart();
            state->since_frame.restart();
        } else if (state->stage == 5 && !editor.autosavePending()) {
            // Verify history against the round-tripped durable representation,
            // not only against the controller's pre-save snapshots.
            if (layers() != state->final) {
                const auto actual = layers();
                double max_coordinate_delta = 0;
                for (qsizetype l = 0; l < std::min(actual.size(), state->final.size()); ++l)
                    for (qsizetype s = 0;
                         s < std::min(actual[l].strokes.size(), state->final[l].strokes.size());
                         ++s) {
                        const auto& a = actual[l].strokes[s].points;
                        const auto& b = state->final[l].strokes[s].points;
                        for (qsizetype p = 0; p < std::min(a.size(), b.size()); ++p)
                            max_coordinate_delta = std::max(
                                {max_coordinate_delta,
                                 std::abs(a[p].x - b[p].x),
                                 std::abs(a[p].y - b[p].y)}
                            );
                    }
                qInfo("Paint persisted coordinate max delta %.17g", max_coordinate_delta);
                fail("autosave changed completed paint");
                return;
            }
            editor.undo();
            if (layers() != state->second) {
                fail("undo stopped working after autosave completed");
                return;
            }
            editor.redo();
            if (layers() != state->final) {
                fail("redo stopped working after autosave completed");
                return;
            }
            qInfo() << "Paint interaction acceptance completed";
            timer->stop();
            pipeline.cancel();
            QCoreApplication::exit(0);
        }
    });
    timer->start();
}
