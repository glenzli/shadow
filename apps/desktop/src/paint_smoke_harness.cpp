#include "paint_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <cmath>
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
            const auto original = layers();
            paint->setBrushSlot(1);
            paint->applyPreset(QStringLiteral("dodge"));
            paint->setRoundness(0.3);
            paint->setAngle(35);
            paint->setSpacing(0.09);
            paint->setTexture(2);
            paint->setTextureStrength(0.7);
            paint->setPressureSize(true);
            paint->setPressureFlow(false);
            paint->setSmoothing(0.6);
            paint->setBrushSlot(0);
            paint->setBrushSlot(1);
            if (layers() != original || paint->roundness() != 0.3 || paint->texture() != 2
                || !paint->pressureSize() || paint->pressureFlow()) {
                fail("A/B brush settings changed existing photo or lost dynamics");
                return;
            }
            if (!paint->beginStroke(0.3, 0.3, 1.5, 0.25)) {
                fail("Dynamic brush stroke not admitted");
                return;
            }
            paint->appendPoint(0.35, 0.35, 0.8);
            paint->appendPoint(0.4, 0.4, 0.5);
            paint->finishStroke();
            const auto dynamic = layers();
            if (dynamic.size() != 2 || dynamic[0] != original[0] || dynamic[1].blend != 2
                || dynamic[1].strokes.size() != 1) {
                fail("New preset blend rewrote old layer instead of creating a new one");
                return;
            }
            const auto& stroke = dynamic[1].strokes[0];
            if (stroke.roundness != 0.3 || stroke.angle_degrees != 35 || stroke.spacing != 0.09
                || stroke.texture != 2 || stroke.texture_strength != 0.7 || !stroke.pressure_size
                || stroke.pressure_flow || stroke.points.first().pressure != 0.25
                || std::abs(stroke.points.last().x - 0.4) > 1e-6
                || std::abs(stroke.points.last().y - 0.4) > 1e-6) {
                fail("Authored dynamics or smoothed endpoint lost");
                return;
            }
            editor.undo();
            if (layers() != original) {
                fail("Dynamic brush undo left its implicit layer");
                return;
            }
            editor.redo();
            if (layers() != dynamic) {
                fail("Dynamic brush redo was not exact");
                return;
            }
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
