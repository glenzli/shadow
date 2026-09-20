#include "paint_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <memory>

void installPaintSmokeHarness(PipelineRunController& pipeline, EditController& editor) {
    auto* const timer = new QTimer(&pipeline);
    timer->setInterval(50);
    struct State {
        int stage = 0, ticks = 0;
        QVector<BackendPaintLayer> saved;
    };
    auto state = std::make_shared<State>();
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, timer, state] {
        const auto fail = [&](const char* message) {
            qCritical() << message;
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (++state->ticks > 1800) {
            fail("Paint acceptance timed out");
            return;
        }
        if (pipeline.finished()) {
            qInfo() << "Paint acceptance completed";
            pipeline.cancel();
            timer->stop();
            return;
        }
        if (pipeline.busy() || editor.busy() || !editor.active()
            || editor.previewSource().isEmpty())
            return;
        auto* const paint = qobject_cast<EditPaintController*>(editor.paint());
        if (!paint) {
            fail("Paint controller missing");
            return;
        }
        const auto layers = [&] { return editor.gradeStackForInterchange().paint_layers; };
        if (state->stage == 0) {
            paint->activate();
            paint->setColor(Qt::white);
            paint->setRadius(0.15);
            paint->setFlow(0.5);
            if (!paint->beginStroke(0.4, 0.5, 1.5)) {
                fail("Paint stroke not admitted");
                return;
            }
            paint->appendPoint(0.6, 0.5);
            paint->finishStroke();
            paint->setBlend(0);
            const auto first = layers();
            if (first.size() != 1 || first[0].strokes.size() != 1) {
                fail("Stroke not recorded");
                return;
            }
            editor.undo(); // blend
            editor.undo(); // entire stroke, including implicit layer creation
            if (!layers().isEmpty()) {
                fail("One stroke undo left partial paint");
                return;
            }
            editor.redo();
            editor.redo();
            if (layers() != first) {
                fail("Paint redo was not exact");
                return;
            }
            if (!paint->beginStroke(0.1, 0.1, 1.5)) {
                fail("Second stroke not admitted");
                return;
            }
            paint->appendPoint(0.2, 0.2);
            paint->cancelStroke();
            if (layers() != first) {
                fail("Cancelled stroke changed durable paint");
                return;
            }
            paint->setErase(true);
            if (!paint->beginStroke(0.5, 0.5, 1.5)) {
                fail("Erase stroke not admitted");
                return;
            }
            paint->finishStroke();
            if (layers()[0].strokes.size() != 2 || !layers()[0].strokes.last().erase) {
                fail("Erase is not a replayable stroke");
                return;
            }
            editor.undo();
            paint->setErase(false);
            paint->setLayerOpacity(0.8);
            state->saved = layers();
            state->stage = 1;
            pipeline.selectPhoto(1);
            return;
        }
        if (state->stage == 1) {
            if (pipeline.currentIndex() != 1)
                return;
            if (!layers().isEmpty()) {
                fail("Paint leaked between photos");
                return;
            }
            paint->activate();
            paint->setColor(QColor(200, 50, 20));
            if (!paint->beginStroke(0.5, 0.5, 1.5)) {
                fail("Second photo stroke not admitted");
                return;
            }
            paint->finishStroke();
            state->stage = 2;
            pipeline.selectPhoto(0);
            return;
        }
        if (state->stage == 2) {
            if (pipeline.currentIndex() != 0)
                return;
            if (layers() != state->saved) {
                fail("Switching lost exact saved paint state");
                return;
            }
            if (!pipeline.configureExport(
                    QUrl::fromLocalFile(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT")),
                    {{QStringLiteral("format"), QStringLiteral("png")}}
                )) {
                fail("Paint export not admitted");
                return;
            }
            qInfo() << "Paint undo/redo/cancel/erase/persistence/isolation checks passed";
            state->stage = 3;
            pipeline.complete();
        }
    });
    timer->start();
}
