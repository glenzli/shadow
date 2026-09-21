#include "pipeline_smoke_harness.hpp"
#include "direct_edit_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "paint_smoke_harness.hpp"
#include "paint_interaction_smoke_harness.hpp"
#include "pipeline_run_controller.hpp"
#include "precision_editing_smoke_harness.hpp"
#include "rgb_curve_smoke_harness.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QWindow>
#include <cmath>
#include <memory>

void installPipelineSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    const QString action = qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_ACTION");
    if (action == QStringLiteral("paint-interaction")) {
        installPaintInteractionSmokeHarness(engine, pipeline, editor);
        return;
    }
    if (action == QStringLiteral("direct-edit")) {
        installDirectEditSmokeHarness(engine, pipeline, editor);
        return;
    }
    if (action == QStringLiteral("rgb-curves")) {
        installRgbCurveSmokeHarness(engine, pipeline, editor);
        return;
    }
    if (action == QStringLiteral("precision")) {
        installPrecisionEditingSmokeHarness(engine, pipeline, editor);
        return;
    }
    if (action == QStringLiteral("paint")) {
        installPaintSmokeHarness(pipeline, editor);
        return;
    }
    if (action.isEmpty())
        return;
    if (action == QStringLiteral("window-chrome")) {
        auto* const window = qobject_cast<QWindow*>(engine.rootObjects().front());
        const auto report = [window] {
            qInfo() << "Pipeline window chrome" << window->geometry() << window->visibility();
        };
        QObject::connect(window, &QWindow::xChanged, window, report);
        QObject::connect(window, &QWindow::yChanged, window, report);
        QObject::connect(window, &QWindow::visibilityChanged, window, report);
        report();
        return;
    }
    auto* const timer = new QTimer(&pipeline);
    timer->setInterval(50);
    const auto stage = std::make_shared<int>(0);
    const auto ticks = std::make_shared<int>(0);
    const auto completed_bytes = std::make_shared<QByteArray>();
    QObject::connect(
        timer,
        &QTimer::timeout,
        &pipeline,
        [&, action, timer, stage, ticks, completed_bytes] {
            if (++*ticks > 1800) {
                qCritical() << "Pipeline smoke timed out" << *stage << pipeline.statusText()
                            << pipeline.errorText();
                timer->stop();
                QCoreApplication::exit(3);
                return;
            }
            if (pipeline.finished()) {
                if (action == QStringLiteral("retry")) {
                    QFile completed(QDir(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT"))
                                        .filePath(QStringLiteral("second-edited.png")));
                    if (*stage != 6 || completed_bytes->isEmpty()
                        || !completed.open(QIODevice::ReadOnly)
                        || completed.readAll() != *completed_bytes) {
                        qCritical() << "Retry changed an already completed output";
                        timer->stop();
                        QCoreApplication::exit(3);
                        return;
                    }
                    qInfo() << "Partial export edit and retry preserved completed bytes";
                }
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
            auto* const window = qobject_cast<QWindow*>(engine.rootObjects().front());
            auto* const title_bar = window->findChild<QObject*>(QStringLiteral("titleToolBar"));
            if (!title_bar || title_bar->property("height").toInt() != 44
                || !window->flags().testFlag(Qt::ExpandedClientAreaHint)
                || !window->flags().testFlag(Qt::NoTitleBarBackgroundHint)) {
                fail("Independent editor must use the shared integrated title bar");
                return;
            }
#if defined(Q_OS_MACOS)
            if (!window->title().isEmpty()) {
                fail("Independent editor must not paint a second native title on macOS");
                return;
            }
#endif
            if (action == QStringLiteral("cancel")) {
                pipeline.cancel();
                timer->stop();
                return;
            }
            if (action != QStringLiteral("complete") && action != QStringLiteral("retry")) {
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
                if (action == QStringLiteral("retry")
                    && !QDir(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT"))
                            .mkdir(QStringLiteral("first-edited-1.png"))) {
                    fail("Could not create the test-only export obstruction");
                    return;
                }
                *stage = 3;
                pipeline.complete();
                return;
            }
            if (action != QStringLiteral("retry"))
                return;
            const QDir output(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT"));
            if (*stage == 3) {
                if (pipeline.completedCount() != 1 || pipeline.currentIndex() != 0
                    || !pipeline.currentPhotoEditable()
                    || !workspace->property("enabled").toBool()) {
                    fail("Failed photo must remain editable after partial export");
                    return;
                }
                QFile completed(output.filePath(QStringLiteral("second-edited.png")));
                if (!completed.open(QIODevice::ReadOnly)) {
                    fail("Successful partial output is missing");
                    return;
                }
                *completed_bytes = completed.readAll();
                *stage = 4;
                pipeline.selectPhoto(1);
                return;
            }
            if (*stage == 4) {
                if (pipeline.currentIndex() != 1)
                    return;
                if (pipeline.currentPhotoEditable() || workspace->property("enabled").toBool()) {
                    fail("Completed photo must not expose edits omitted by retry");
                    return;
                }
                *stage = 5;
                pipeline.selectPhoto(0);
                return;
            }
            if (*stage == 5) {
                if (pipeline.currentIndex() != 0)
                    return;
                if (!pipeline.currentPhotoEditable() || !workspace->property("enabled").toBool()
                    || !output.rmdir(QStringLiteral("first-edited-1.png"))) {
                    fail("Failed photo did not recover for retry");
                    return;
                }
                editor.setExposureStops(0.75);
                *stage = 6;
                pipeline.complete();
            }
        }
    );
    timer->start();
}
