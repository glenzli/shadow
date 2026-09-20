#include "pipeline_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <cmath>
#include <memory>

void installPipelineSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    const QString action = qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_ACTION");
    if (action.isEmpty())
        return;
    auto* const timer = new QTimer(&pipeline);
    timer->setInterval(50);
    const auto stage = std::make_shared<int>(0);
    const auto ticks = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, action, timer, stage, ticks] {
        if (++*ticks > 1800) {
            qCritical() << "Pipeline smoke timed out" << *stage << pipeline.statusText()
                        << pipeline.errorText();
            timer->stop();
            QCoreApplication::exit(3);
            return;
        }
        if (pipeline.finished()) {
            qInfo() << "Pipeline smoke completed with preserved per-photo adjustments";
            pipeline.cancel();
            timer->stop();
            return;
        }
        auto* const workspace =
            engine.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (pipeline.busy() || editor.busy() || !editor.active() || !workspace
            || !workspace->property("previewFrameReady").toBool())
            return;
        const auto fail = [&](const char* message) {
            qCritical() << message;
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (action == QStringLiteral("cancel")) {
            pipeline.cancel();
            timer->stop();
            return;
        }
        if (action != QStringLiteral("complete")) {
            fail("Unknown pipeline smoke action");
            return;
        }
        if (*stage == 0) {
            editor.setExposureStops(1.25);
            *stage = 1;
            if (pipeline.photoCount() > 1)
                pipeline.selectPhoto(1);
            return;
        }
        if (*stage == 1 && pipeline.photoCount() > 1) {
            if (pipeline.currentIndex() != 1)
                return;
            if (std::abs(editor.exposureStops()) > 0.001) {
                fail("Edits leaked across photos");
                return;
            }
            editor.setExposureStops(-0.5);
            *stage = 2;
            pipeline.selectPhoto(0);
            return;
        }
        if (*stage == 2) {
            if (pipeline.currentIndex() != 0)
                return;
            if (std::abs(editor.exposureStops() - 1.25) > 0.001) {
                fail("Switching lost the first photo's adjustments");
                return;
            }
        }
        if (*stage < 3) {
            if (pipeline.interactive()
                && !pipeline.configureExport(
                    QUrl::fromLocalFile(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT")),
                    {{QStringLiteral("format"), QStringLiteral("png")}}
                )) {
                fail("Could not configure interactive export");
                return;
            }
            qInfo() << "Pipeline smoke edit/switch checks passed";
            *stage = 3;
            pipeline.complete();
        }
    });
    timer->start();
}
