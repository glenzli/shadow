#include "raw_white_balance_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "pipeline_run_controller.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QTimer>
#include <QWindow>
#include <memory>
#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QTest>
#endif

void installRawWhiteBalanceSmokeHarness(
    QQmlApplicationEngine& engine, PipelineRunController& pipeline, EditController& editor
) {
    struct State {
        int stage = 0, temperature = 0, tint = 0;
        QString preview;
        QElapsedTimer lifetime;
    };
    auto state = std::make_shared<State>();
    state->lifetime.start();
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, state, timer] {
        const auto fail = [timer, state](const char* reason) {
            qCritical() << "RAW white balance acceptance failed" << state->stage << reason;
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->lifetime.elapsed() > 240000) {
            fail("timeout");
            return;
        }
        if (pipeline.finished()) {
            if (state->stage != 4 || pipeline.completedCount() != pipeline.photoCount()
                || !pipeline.errorText().isEmpty()) {
                fail("editing session did not complete its export");
                return;
            }
            qInfo() << "RAW white balance acceptance passed: visible controls, pointer edits, "
                       "camera reset, latest preview and export"
                    << state->temperature << state->tint;
            timer->stop();
            pipeline.cancel();
            return;
        }
        auto* root = engine.rootObjects().front();
        auto* workspace = root->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (pipeline.busy() || editor.busy() || !editor.active() || !workspace
            || !workspace->property("previewFrameReady").toBool()) {
            return;
        }
        auto* window = qobject_cast<QWindow*>(root);
        const auto click_slider = [window, workspace](const char* name, const double position) {
#ifdef SHADOW_DESKTOP_UI_SMOKE
            auto* field = workspace->findChild<QQuickItem*>(QString::fromLatin1(name));
            if (!field || !field->isVisible() || !field->isEnabled()) {
                return false;
            }
            for (auto* candidate : field->findChildren<QQuickItem*>()) {
                if (!candidate->property("pressed").isValid()
                    || !candidate->property("from").isValid()) {
                    continue;
                }
                const auto point = candidate->mapToScene(
                    {candidate->width() * position, candidate->height() / 2.0}
                ).toPoint();
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
                return true;
            }
#else
            Q_UNUSED(window);
            Q_UNUSED(workspace);
            Q_UNUSED(name);
            Q_UNUSED(position);
#endif
            return false;
        };
        if (state->stage == 0) {
            if (!editor.foundationWhiteBalanceCameraValueAvailable()
                || !editor.foundationWhiteBalanceAtCameraValue()) {
                qCritical() << "RAW camera white balance state"
                            << editor.foundationWhiteBalanceCameraValueAvailable()
                            << editor.foundationWhiteBalanceAtCameraValue()
                            << editor.foundationWhiteBalanceTemperature()
                            << editor.foundationWhiteBalanceTint() << editor.previewSource()
                            << editor.statusText();
                fail("camera calibration did not reach the RAW controls");
                return;
            }
            state->temperature = editor.foundationWhiteBalanceTemperature();
            state->tint = editor.foundationWhiteBalanceTint();
            state->preview = editor.previewSource();
            if (!click_slider("foundationWhiteBalanceTemperatureSlider", 0.30)
                || editor.foundationWhiteBalanceAtCameraValue()) {
                fail("temperature pointer input is unavailable");
                return;
            }
            state->stage = 1;
            return;
        }
        if (editor.previewSource() == state->preview) {
            return;
        }
        if (state->stage == 1) {
            state->preview = editor.previewSource();
            const int previous_tint = editor.foundationWhiteBalanceTint();
            if (!click_slider("foundationWhiteBalanceTintSlider", 0.57)
                || editor.foundationWhiteBalanceTint() == previous_tint) {
                fail("tint pointer input is unavailable");
                return;
            }
            state->stage = 2;
            return;
        }
        if (state->stage == 2) {
            state->preview = editor.previewSource();
#ifdef SHADOW_DESKTOP_UI_SMOKE
            auto* reset = workspace->findChild<QQuickItem*>("foundationWhiteBalanceResetButton");
            if (!reset || !reset->isVisible() || !reset->isEnabled()) {
                fail("camera reset button is unavailable");
                return;
            }
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                reset->mapToScene({reset->width() / 2.0, reset->height() / 2.0}).toPoint());
#endif
            state->stage = 3;
            return;
        }
        if (state->stage == 3) {
            if (!editor.foundationWhiteBalanceAtCameraValue()
                || editor.foundationWhiteBalanceTemperature() != state->temperature
                || editor.foundationWhiteBalanceTint() != state->tint) {
                fail("camera reset changed the recorded white point");
                return;
            }
            qInfo() << "RAW camera white balance restored" << state->temperature << state->tint;
            pipeline.complete();
            state->stage = 4;
        }
    });
    timer->start();
}
