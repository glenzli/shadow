#include "precision_editing_smoke_harness.hpp"
#include "edit_condition_mask_controller.hpp"
#include "edit_controller.hpp"
#include "edit_retouch_sources.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrlQuery>
#include <memory>

void installPrecisionEditingSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    struct State {
        int stage = 0;
        int reported_stage = -1;
        QElapsedTimer elapsed;
        QElapsedTimer gesture;
        BackendGradeStack saved;
        QString awaited_source;
    };
    const auto state = std::make_shared<State>();
    state->elapsed.start();
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(30);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, state, timer] {
        if (state->stage != state->reported_stage) {
            state->reported_stage = state->stage;
            qInfo() << "Precision acceptance stage" << state->stage;
        }
        const auto fail = [&](const char* message) {
            qCritical() << message << state->stage << editor.statusText() << "mask active"
                        << editor.maskToolActive() << "coverage"
                        << !editor.maskCoverageSource().isEmpty();
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->elapsed.elapsed() > 240000) {
            fail("Precision editing acceptance timed out");
            return;
        }
        if (pipeline.finished()) {
            qInfo() << "Precision editing acceptance completed";
            timer->stop();
            pipeline.cancel();
            return;
        }
        if (state->stage == 7 && !pipeline.busy() && !pipeline.errorText().isEmpty()) {
            qCritical() << pipeline.errorText();
            fail("Precision export failed");
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
            if (!editor.createLocalMask(7, 0)) {
                fail("Combined mask creation failed");
                return;
            }
            if (workspace->property("activeSpecialTool").toInt() != 1)
                QMetaObject::invokeMethod(workspace, "setActiveSpecialTool", Q_ARG(QVariant, 1));
            editor.setMaskToolActive(true);
            editor.setExposureStops(1);
            state->stage = 1;
            return;
        case 1: {
            if (editor.maskCoverageSource().isEmpty())
                return;
            auto* overlay = workspace->findChild<QObject*>("precisionMaskCoverageOverlay");
            if (!overlay || !overlay->property("coverageReady").toBool()
                || !overlay->property("visible").toBool())
                return;
            const auto before = editor.gradeStackForInterchange();
            state->gesture.start();
            editor.beginParameterEdit("local_mask/conditions");
            for (int i = 0; i < 20; ++i) {
                auto expression =
                    QJsonDocument::fromJson(defaultConditionMaskExpression().toUtf8()).object();
                auto root = expression["root"].toObject();
                auto children = root["children"].toArray();
                auto leaf = children[0].toObject();
                auto condition = leaf["condition"].toObject();
                condition["lower"] = 0.21 + i * 0.02;
                leaf["condition"] = condition;
                children[0] = leaf;
                root["children"] = children;
                expression["root"] = root;
                editor.setSelectedConditionMask(
                    QString::fromUtf8(QJsonDocument(expression).toJson(QJsonDocument::Compact))
                );
            }
            editor.endParameterEdit("local_mask/conditions");
            const auto after = editor.gradeStackForInterchange();
            editor.undo();
            if (editor.gradeStackForInterchange() != before) {
                fail("Condition drag did not undo atomically");
                return;
            }
            editor.redo();
            if (editor.gradeStackForInterchange() != after) {
                fail("Condition redo changed the expression");
                return;
            }
            state->stage = 2;
            return;
        }
        case 2:
            if (editor.maskCoverageSource().isEmpty())
                return;
            qInfo() << "Condition burst plus undo/redo settled ms" << state->gesture.elapsed();
            editor.setSelectedLocalMaskInverted(true);
            state->stage = 3;
            return;
        case 3:
            if (editor.maskCoverageSource().isEmpty())
                return;
            editor.setMaskCoverageShowsSelectedComponent(false);
            state->stage = 4;
            return;
        case 4:
            if (editor.maskCoverageSource().isEmpty())
                return;
            QMetaObject::invokeMethod(workspace, "setActiveSpecialTool", Q_ARG(QVariant, 3));
            editor.setRetouchCreationMode(0);
            editor.setRetouchBrushRadius(64);
            editor.clearRetouchSource();
            editor.addRetouchStrokeFromPreview(
                {QVariantMap{{"x", 0.35}, {"y", 0.25}}},
                QUrlQuery(QUrl(editor.previewSource())).queryItemValue("generation"),
                int(editor.levelZeroWidth()),
                int(editor.levelZeroHeight())
            );
            state->stage = 8;
            return;
        case 8: {
            const auto before = editor.gradeStackForInterchange();
            if (before.retouch_strokes.size() != 1
                || (before.retouch_strokes[0].source_offset_x_radii == 0
                    && before.retouch_strokes[0].source_offset_y_radii == 0)) {
                fail("Settled preview did not provide an automatic donor");
                return;
            }
            auto* sources = qobject_cast<EditRetouchSources*>(editor.retouchSources());
            sources->nextCandidate(true, 0);
            const auto after = editor.gradeStackForInterchange();
            if (after == before) {
                fail("Candidate cycling did not change the source");
                return;
            }
            editor.undo();
            if (editor.gradeStackForInterchange() != before) {
                fail("Source cycling undo was not exact");
                return;
            }
            editor.redo();
            if (editor.gradeStackForInterchange() != after) {
                fail("Source cycling redo was not exact");
                return;
            }
            editor.setRetouchSourceFromPreview(0.34, 0.25);
            sources->remember();
            editor.clearRetouchSource();
            sources->recall(0);
            if (!editor.retouchSourceSampled()
                || editor.retouchSampledSource().value("x").toDouble() != 0.34) {
                fail("Remembered source was not restored");
                return;
            }
            state->stage = 9;
            return;
        }
        case 9: {
            state->awaited_source = editor.previewSource();
            editor.setRetouchStrokeMode(0, 3);
            auto* sources = qobject_cast<EditRetouchSources*>(editor.retouchSources());
            sources->setRegionFrequency(true, 0, 12);
            editor.undo();
            if (editor.gradeStackForInterchange().retouch_strokes[0].frequency_radius != 8) {
                fail("Frequency scale undo did not restore the prior value");
                return;
            }
            editor.redo();
            if (editor.gradeStackForInterchange().retouch_strokes[0].frequency_radius != 12) {
                fail("Frequency scale redo did not restore the authored value");
                return;
            }
            state->gesture.restart();
            state->stage = 10;
            return;
        }
        case 10:
            if (editor.previewSource() == state->awaited_source)
                return;
            state->awaited_source = editor.previewSource();
            qInfo() << "Tone repair settled ms" << state->gesture.elapsed();
            editor.setRetouchCreationMode(4);
            editor.setRetouchSourceFromPreview(0.37, 0.25);
            editor.addRetouchStrokeFromPreview(
                {QVariantMap{{"x", 0.38}, {"y", 0.25}}},
                QUrlQuery(QUrl(editor.previewSource())).queryItemValue("generation"),
                int(editor.levelZeroWidth()),
                int(editor.levelZeroHeight())
            );
            state->gesture.restart();
            state->stage = 11;
            return;
        case 11:
            if (editor.previewSource() == state->awaited_source)
                return;
            if (editor.gradeStackForInterchange().retouch_strokes.size() != 2
                || editor.gradeStackForInterchange().retouch_strokes[1].mode != 4) {
                fail("Texture repair was not authored");
                return;
            }
            qInfo() << "Combined tone and texture repair settled ms" << state->gesture.elapsed();
            state->awaited_source = editor.previewSource();
            editor.beginParameterEdit("retouch/stroke/1/frequency");
            for (int i = 2; i <= 21; ++i)
                qobject_cast<EditRetouchSources*>(editor.retouchSources())
                    ->setRegionFrequency(true, 1, i);
            editor.endParameterEdit("retouch/stroke/1/frequency");
            state->gesture.restart();
            state->stage = 12;
            return;
        case 12:
            if (editor.previewSource() == state->awaited_source)
                return;
            qInfo() << "Warm 20-value frequency gesture settled ms" << state->gesture.elapsed();
            state->saved = editor.gradeStackForInterchange();
            editor.setMaskToolActive(false);
            state->stage = 5;
            pipeline.selectPhoto(1);
            return;
        case 5:
            if (pipeline.currentIndex() != 1)
                return;
            if (qobject_cast<EditRetouchSources*>(editor.retouchSources())
                    ->saved()
                    .first()
                    .toBool()) {
                fail("Remembered source leaked to another photo");
                return;
            }
            if (!editor.gradeStackForInterchange()
                     .grade_nodes.front()
                     .local_mask_components.isEmpty()) {
                fail("Mask leaked across photos");
                return;
            }
            state->stage = 6;
            pipeline.selectPhoto(0);
            return;
        case 6:
            if (pipeline.currentIndex() != 0)
                return;
            if (editor.gradeStackForInterchange() != state->saved) {
                fail("Condition state changed after reopening photo");
                return;
            }
            if (!pipeline.configureExport(
                    QUrl::fromLocalFile(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT")),
                    {{QStringLiteral("format"), QStringLiteral("jpeg")}}
                )) {
                fail("Precision export rejected");
                return;
            }
            state->stage = 7;
            qInfo() << "Condition coverage, atomic undo, composite inversion, isolation and "
                       "persistence passed";
            pipeline.complete();
            return;
        default:
            return;
        }
    });
    timer->start();
}
