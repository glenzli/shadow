#include "auto_start_smoke_harness.hpp"
#include "edit_auto_start_controller.hpp"
#include "edit_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrl>
#include <memory>
namespace {
struct State {
    int stage = 0;
    BackendGradeStack before, after;
    bool undo = false, fast = false, automatic = false;
    QString candidate, source;
    QElapsedTimer elapsed;
};
} // namespace
void installAutoStartSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    auto* feature = qobject_cast<EditAutoStartController*>(editor.autoStart());
    auto state = std::make_shared<State>();
    state->elapsed.start();
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, feature, state, timer] {
        const auto fail = [&](const QString& message) {
            qCritical() << "Auto start acceptance failed" << state->stage << message
                        << feature->status() << editor.statusText();
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (state->elapsed.elapsed() > 300000) {
            fail(QStringLiteral("timeout"));
            return;
        }
        if (pipeline.finished()) {
            qInfo() << "Auto start export completed";
            timer->stop();
            pipeline.cancel();
            return;
        }
        if (editor.autosaveFailed()) {
            fail(QStringLiteral("autosave failed"));
            return;
        }
        if (!editor.active())
            return;
        auto* workspace =
            engine.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (!workspace || !workspace->property("previewFrameReady").toBool())
            return;
        if (state->stage == 20) {
            if (!feature->active())
                return;
            if (editor.gradeStackForInterchange() != state->before) {
                fail(QStringLiteral("automatic preparation mutated edits"));
                return;
            }
            feature->setAutomaticEnabled(false);
            feature->cancel();
            state->stage = 21;
            return;
        }
        if (state->stage == 2 && feature->ready() && !state->fast) {
            qInfo() << "Auto start first measured preview" << state->elapsed.elapsed() << "ms";
            state->fast = true;
        }
        if (state->stage == 2 && qEnvironmentVariableIsSet("SHADOW_AUTO_START_SMOKE_EARLY_APPLY")
            && feature->canApply() && feature->analyzing()) {
            qInfo() << "Applying measured preview before AI completion";
            feature->apply();
            state->stage = 5;
            return;
        }
        if (feature->busy() || editor.busy() || editor.autosavePending())
            return;
        switch (state->stage) {
        case 0:
            state->before = editor.gradeStackForInterchange();
            state->undo = editor.canUndo();
            if (qEnvironmentVariableIsSet("SHADOW_AUTO_START_SMOKE_LIFECYCLE")) {
                if (pipeline.photoCount() < 2) {
                    fail(QStringLiteral("lifecycle acceptance requires two photos"));
                    return;
                }
                state->source = editor.sourcePath();
                state->automatic = feature->automaticEnabled();
                feature->setAutomaticEnabled(true);
                state->stage = 20;
                return;
            }
            feature->analyze();
            feature->cancel();
            state->stage = 1;
            return;
        case 1:
            if (feature->active() || editor.gradeStackForInterchange() != state->before
                || editor.canUndo() != state->undo) {
                fail(QStringLiteral("cancel mutated edits"));
                return;
            }
            feature->analyze();
            state->stage = 2;
            return;
        case 2: {
            if (!feature->ready() || feature->previewSource().isEmpty()
                || editor.gradeStackForInterchange() != state->before) {
                fail(QStringLiteral("proposal must remain disposable"));
                return;
            }
            const QString evidence = qEnvironmentVariable("SHADOW_AUTO_START_EVIDENCE_DIR");
            if (!evidence.isEmpty() && QDir(evidence).exists()) {
                const auto save = [&](const QString& source, const QString& name) {
                    QFile file(QDir(evidence).filePath(name));
                    if (file.open(QIODevice::WriteOnly))
                        file.write(
                            QByteArray::fromBase64(
                                source.mid(source.indexOf(QLatin1Char(',')) + 1).toLatin1()
                            )
                        );
                };
                save(feature->originalSource(), QStringLiteral("before.jpg"));
                save(feature->previewSource(), QStringLiteral("candidate.jpg"));
            }
            qInfo() << "Auto start analysis:" << feature->status() << feature->summary()
                    << state->elapsed.elapsed() << "ms";
            if (qEnvironmentVariableIsSet("SHADOW_AUTO_START_REQUIRE_AI")
                && !feature->sceneAnalyzed()) {
                fail(QStringLiteral("local scene model unavailable"));
                return;
            }
            feature->setStrength(0);
            state->stage = 3;
            return;
        }
        case 3:
            if (feature->canApply()) {
                fail(QStringLiteral("zero strength must not publish"));
                return;
            }
            feature->setStrength(.65);
            state->stage = 4;
            return;
        case 4:
            if (!feature->canApply()) {
                fail(QStringLiteral("no useful proposal for selected fixture"));
                return;
            }
            state->candidate = feature->previewSource();
            feature->apply();
            state->stage = 5;
            return;
        case 5:
            state->after = editor.gradeStackForInterchange();
            if (state->before == state->after || !editor.canUndo()) {
                fail(QStringLiteral("apply did not add editable adjustments"));
                return;
            }
            editor.undo();
            state->stage = 6;
            return;
        case 6:
            if (editor.gradeStackForInterchange() != state->before) {
                fail(QStringLiteral("one undo did not restore all starting values"));
                return;
            }
            editor.redo();
            state->stage = 7;
            return;
        case 7:
            if (editor.gradeStackForInterchange() != state->after) {
                fail(QStringLiteral("redo did not restore exact adjustments"));
                return;
            }
            qInfo() << "Auto start acceptance passed: cancel, temporary preview, atomic apply, "
                       "undo, redo"
                    << state->elapsed.elapsed() << "ms";
            state->stage = 8;
            if (!pipeline.configureExport(
                    QUrl::fromLocalFile(qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_OUTPUT")),
                    {{QStringLiteral("format"), QStringLiteral("jpeg")}}
                )) {
                fail(QStringLiteral("output unavailable"));
                return;
            }
            pipeline.complete();
            return;
        case 21:
            feature->analyze();
            if (!feature->active()) {
                fail(QStringLiteral("manual analysis did not start"));
                return;
            }
            editor.setExposureStops(editor.exposureStops() + .1);
            if (feature->active()) {
                fail(QStringLiteral("manual edit retained a stale proposal"));
                return;
            }
            state->stage = 22;
            return;
        case 22:
            editor.undo();
            state->stage = 23;
            return;
        case 23:
            if (editor.gradeStackForInterchange() != state->before) {
                fail(QStringLiteral("cancelled analysis interfered with manual undo"));
                return;
            }
            feature->analyze();
            if (!feature->active()) {
                fail(QStringLiteral("analysis before photo switch did not start"));
                return;
            }
            pipeline.selectPhoto(1);
            state->stage = 24;
            return;
        case 24:
            if (pipeline.currentIndex() != 1 || editor.sourcePath() == state->source)
                return;
            if (feature->active() || feature->canApply()) {
                fail(QStringLiteral("photo switch retained a stale proposal"));
                return;
            }
            feature->setAutomaticEnabled(state->automatic);
            qInfo()
                << "Auto start lifecycle passed: automatic preparation, manual edit cancellation,"
                   " manual undo, photo-switch cancellation"
                << state->elapsed.elapsed() << "ms";
            timer->stop();
            pipeline.cancel();
            return;
        default:
            return;
        }
    });
    timer->start();
}
