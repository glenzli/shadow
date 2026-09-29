#include "color_warper_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <memory>

namespace {
double coordinate(QObject* grid, const char* method) {
    QVariant value;
    QMetaObject::invokeMethod(grid, method, Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, 12));
    return value.toDouble();
}
} // namespace

void installColorWarperSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    struct State {
        int stage = 0, moves = 0;
        BackendGradeStack before, after;
        QElapsedTimer elapsed, gesture;
        double x = 0, y = 0;
        QString source;
    };
    const auto state = std::make_shared<State>();
    state->elapsed.start();
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, state, timer] {
        const auto fail = [&](const char* message) {
            qCritical() << "Color map acceptance failed" << state->stage << message
                        << editor.statusText();
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->elapsed.elapsed() > 180000) {
            fail("timeout");
            return;
        }
        auto* workspace =
            engine.rootObjects().front()->findChild<QQuickItem*>("pipelinePrecisionWorkspace");
        if (!workspace || !editor.active())
            return;
        auto* panel = workspace->findChild<QQuickItem*>("precisionColorWarperPanel");
        auto* grid = workspace->findChild<QQuickItem*>("expandedColorWarperEditor");
        auto* canvas = workspace->findChild<QQuickItem*>("precisionCanvas");
        auto* window = workspace->window();
        if (!panel || !grid || !canvas || !window) {
            fail("packaged workspace wiring");
            return;
        }
        if (state->stage == 2) {
            // Keep feeding the existing latest-value-wins path during preview work.
            ++state->moves;
            QMetaObject::invokeMethod(
                grid,
                "movePointGesture",
                Q_ARG(QVariant, state->x + state->moves),
                Q_ARG(QVariant, state->y),
                Q_ARG(QVariant, int(Qt::ShiftModifier))
            );
            if (state->moves == 12) {
                QMetaObject::invokeMethod(grid, "finishEditing");
                state->stage = 3;
            }
            return;
        }
        if (pipeline.busy() || editor.busy() || editor.autosavePending()
            || !workspace->property("previewFrameReady").toBool())
            return;
        const auto geometryValid = [&] {
            const auto photo = canvas->mapRectToItem(workspace, canvas->boundingRect());
            const auto controls = panel->mapRectToItem(workspace, panel->boundingRect());
            return panel->isVisible() && photo.right() <= controls.left() + 1
                   && photo.width() >= workspace->width() * .5 - 1
                   && controls.right() <= workspace->width() + 1;
        };
        const auto capture = [&](const char* name) {
            const QString directory = qEnvironmentVariable("SHADOW_WARPER_EVIDENCE_DIR");
            if (directory.isEmpty())
                return true;
            return window->grabWindow()
                .scaled(1440, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                .save(QDir(directory).filePath(QString::fromLatin1(name)));
        };
        switch (state->stage) {
        case 0:
            if (pipeline.photoCount() < 2) {
                fail("requires two named RAW inputs");
                return;
            }
            editor.selectGradeNode(0);
            state->before = editor.gradeStackForInterchange();
            state->source = editor.sourcePath();
            window->resize(1440, 900);
            QMetaObject::invokeMethod(workspace, "openColorWarper");
            qInfo() << "Color map cold preview ready" << state->elapsed.elapsed() << "ms";
            state->stage = 1;
            return;
        case 1: {
            auto* frame = grid->findChild<QQuickItem*>("colorWarperFrame");
            if (!geometryValid() || !frame || frame->width() < 350
                || workspace->findChild<QQuickItem*>("precisionGradeNodePane")->isVisible()
                || workspace->findChild<QQuickItem*>("precisionInspector")->isVisible()) {
                fail("expanded dock obscures or crowds the photograph");
                return;
            }
            state->x = coordinate(grid, "pointX");
            state->y = coordinate(grid, "pointY");
            QMetaObject::invokeMethod(
                grid,
                "beginPointGesture",
                Q_ARG(QVariant, 12),
                Q_ARG(QVariant, state->x),
                Q_ARG(QVariant, state->y)
            );
            if (editor.gradeStackForInterchange() != state->before) {
                fail("press moved the point");
                return;
            }
            state->gesture.start();
            state->stage = 2;
            return;
        }
        case 3:
            state->after = editor.gradeStackForInterchange();
            if (state->after == state->before || !geometryValid()) {
                fail("live gesture state");
                return;
            }
            qInfo() << "Color map 12-sample warm gesture and settled frame"
                    << state->gesture.elapsed() << "ms";
            if (!capture("warper-wide.png")) {
                fail("wide capture");
                return;
            }
            editor.undo();
            state->stage = 4;
            return;
        case 4:
            if (editor.gradeStackForInterchange() != state->before) {
                fail("one undo must restore the gesture");
                return;
            }
            editor.redo();
            state->stage = 5;
            return;
        case 5:
            if (editor.gradeStackForInterchange() != state->after) {
                fail("exact redo");
                return;
            }
            window->resize(1000, 760);
            workspace->setProperty("colorWarperPanelWidth", 10000);
            state->stage = 6;
            return;
        case 6:
            if (!geometryValid() || !capture("warper-narrow.png")) {
                fail("narrow photo/controls separation");
                return;
            }
            QMetaObject::invokeMethod(workspace, "closeColorWarper");
            state->stage = 7;
            return;
        case 7:
            if (panel->isVisible()
                || !workspace->findChild<QQuickItem*>("precisionInspector")->isVisible()
                || !workspace->findChild<QQuickItem*>("precisionGradeNodePane")->isVisible()) {
                fail("closing must restore the standard workspace");
                return;
            }
            QMetaObject::invokeMethod(workspace, "openColorWarper");
            pipeline.selectPhoto(1);
            state->stage = 8;
            return;
        case 8:
            if (editor.sourcePath() == state->source)
                return;
            if (panel->isVisible()) {
                fail("photo switch retained a stale panel");
                return;
            }
            qInfo() << "Color map acceptance passed: real RAW, non-overlapping wide/narrow layout, "
                       "warm gesture, undo/redo, restore and photo isolation";
            timer->stop();
            pipeline.cancel();
            return;
        }
    });
    timer->start();
}
