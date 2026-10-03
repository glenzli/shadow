#include "edit_tool_ui_smoke.hpp"
#include "edit_controller.hpp"
#include "edit_tool_controller.hpp"
#include "pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include <stdexcept>
#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QTest>
#endif

namespace {
struct State {
    std::shared_ptr<EditToolController> tools;
    QMap<QString, QJsonObject> replies;
    QJsonObject snapshot, proposal, receipt, export_receipt;
    QString pending;
    bool cancel_checked = false;
    int step = 0, requests = 0, ticks = 0;
};
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
QString send(State& state, const QString& op, const QJsonObject& params = {}) {
    const auto id = QString::number(++state.requests);
    state.tools->receive(QJsonDocument(
                             QJsonObject{
                                 {"schema", EditToolProtocol::schema},
                                 {"id", id},
                                 {"op", op},
                                 {"params", params}
                             }
    ).toJson(QJsonDocument::Compact));
    return id;
}
QJsonObject take(State& state, const QString& id) {
    const auto response = state.replies.take(id);
    require(!response.isEmpty(), "missing synchronous owner reply");
    return response;
}
QJsonObject ok(State& state, const QString& id) {
    const auto response = take(state, id);
    if (!response.value("ok").toBool())
        throw std::runtime_error(
            QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString()
        );
    return response.value("result").toObject();
}
void rejected(State& state, const QString& id, const QString& code) {
    const auto response = take(state, id);
    require(
        !response.value("ok").toBool()
            && response.value("error").toObject().value("code").toString() == code,
        "owner did not reject the unsafe tool request"
    );
}
void click(QQuickWindow& window, const char* name) {
    auto* item = window.findChild<QQuickItem*>(QString::fromLatin1(name));
    require(item && item->isVisible() && item->isEnabled(), "real packaged control is unavailable");
#ifdef SHADOW_DESKTOP_UI_SMOKE
    const auto point = item->mapToScene({item->width() / 2, item->height() / 2}).toPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point);
    require(item->property("down").toBool(), "real window pointer did not arm control");
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
#else
    throw std::runtime_error("real pointer acceptance requires BUILD_TESTING");
#endif
}
void capture(QQuickWindow& window, const QDir& root, const QString& name) {
    require(
        window.grabWindow().save(root.filePath(name)),
        "could not capture this application window"
    );
}
struct HoldWorkers {
    HoldWorkers() {
        QThreadPool::globalInstance()->reserveThread();
        QThreadPool::globalInstance()->reserveThread();
    }
    ~HoldWorkers() {
        QThreadPool::globalInstance()->releaseThread();
        QThreadPool::globalInstance()->releaseThread();
    }
};
} // namespace

void installEditToolUiSmoke(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor,
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& source
) {
    const QDir root(qEnvironmentVariable("SHADOW_AGENT_TOOL_EVIDENCE_DIR"));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (!window || !root.exists()) {
        QCoreApplication::exit(3);
        return;
    }
    QThreadPool::globalInstance()->setMaxThreadCount(2);
    auto state = std::make_shared<State>();
    state->tools = std::make_shared<EditToolController>(editor, backend, source);
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(50);
    QObject::connect(
        state->tools.get(),
        &EditToolController::reply,
        timer,
        [weak = std::weak_ptr<State>(state)](const QJsonObject& response) {
            if (auto current = weak.lock())
                current->replies.insert(response.value("id").toString(), response);
        }
    );
    QObject::connect(timer, &QTimer::timeout, timer, [&, state, timer, window, root] {
        const auto record = [&](const QString& result, const QString& error = {}) {
            QFile file(root.filePath("owner-ui-result.json"));
            if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                file.write(QJsonDocument(
                               QJsonObject{
                                   {"result", result},
                                   {"step", state->step},
                                   {"editorActive", editor.active()},
                                   {"editorBusy", editor.busy()},
                                   {"editorStatus", editor.statusText()},
                                   {"pipelineStatus", pipeline.statusText()},
                                   {"pipelineError", pipeline.errorText()},
                                   {"error", error},
                                   {"applyReceipt", state->receipt},
                                   {"exportReceipt", state->export_receipt},
                                   {"platform", QGuiApplication::platformName()},
                                   {"evidence",
                                    QJsonArray{
                                        "snapshot_preserves_dirty_gesture",
                                        "held_preview_cancel_no_artifact",
                                        "held_apply_export_cancel_unsupported",
                                        "same_head_variant_rejected",
                                        "held_apply_blocks_close_and_edits",
                                        "actual_pointer_undo_redo",
                                        "export_blocks_second_runner",
                                        "human_edit_during_pinned_export",
                                        "shutdown_reserves_clean_draft"
                                    }}
                               }
                ).toJson());
        };
        try {
            require(++state->ticks < 1800, "owner/UI acceptance timed out");
            if (!state->pending.isEmpty()) {
                if (!state->replies.contains(state->pending))
                    return;
            } else if (
                pipeline.busy() || editor.busy() || !editor.active() || editor.dirty()
                || editor.autosavePending()
            ) {
                return;
            }
            auto& s = *state;
            const auto preview = [&] {
                const auto id = s.snapshot.value("identity").toObject();
                const auto node = s.snapshot.value("nodes").toArray().first().toObject();
                return send(
                    s,
                    "preview",
                    {{"expected", id},
                     {"operation",
                      QJsonObject{
                          {"type", "set_exposure"},
                          {"nodeId", node.value("nodeId")},
                          {"stops", 1.0}
                      }},
                     {"outputPath",
                      root.filePath(QStringLiteral("owner-preview-%1.jpg").arg(s.step))}}
                );
            };
            switch (s.step) {
            case 0: {
                auto* workspace = window->findChild<QObject*>("pipelinePrecisionWorkspace");
                if (!workspace || !workspace->property("previewFrameReady").toBool())
                    return;
                window->requestActivate();
                editor.beginParameterEdit("exposure");
                editor.setExposureStops(0.4);
                s.snapshot = ok(s, send(s, "snapshot"));
                require(
                    s.snapshot.value("dirty").toBool()
                        && s.snapshot.value("gestureActive").toBool(),
                    "snapshot finalized or saved a human gesture"
                );
                rejected(s, preview(), "draft_pending");
                require(editor.exposureStops() == 0.4, "rejected tool changed the human draft");
                editor.endParameterEdit("exposure");
                ++s.step;
                break;
            }
            case 1:
                click(*window, "pipelineUndoButton");
                ++s.step;
                break;
            case 2:
                require(editor.exposureStops() == 0.0, "actual Undo did not restore seed");
                if (!s.cancel_checked) {
                    s.snapshot = ok(s, send(s, "snapshot"));
                    HoldWorkers held;
                    s.pending = preview();
                    const auto result = ok(s, send(s, "cancel", {{"requestId", s.pending}}));
                    require(result.value("accepted").toBool(), "held preview was not cancelled");
                    s.cancel_checked = true;
                    break;
                }
                rejected(s, s.pending, "cancelled");
                s.pending.clear();
                require(
                    !QFile::exists(root.filePath("owner-preview-2.jpg")),
                    "cancelled preview published an artifact"
                );
                s.snapshot = ok(s, send(s, "snapshot"));
                s.pending = preview();
                ++s.step;
                break;
            case 3: {
                s.proposal = ok(s, s.pending);
                s.pending.clear();
                const auto old = s.snapshot.value("identity").toObject();
                editor.createVariant("Human alternate");
                require(
                    editor.durableWorkingCommitId() == old.value("workingCommitId").toString()
                        && editor.activeVariantId() != old.value("activeVariantId").toString(),
                    "same-head Variant fixture did not switch identity"
                );
                rejected(
                    s,
                    send(
                        s,
                        "apply",
                        {{"expected", old}, {"proposalId", s.proposal.value("proposalId")}}
                    ),
                    "stale_snapshot"
                );
                ++s.step;
                break;
            }
            case 4:
                s.snapshot = ok(s, send(s, "snapshot"));
                s.pending = preview();
                ++s.step;
                break;
            case 5: {
                s.proposal = ok(s, s.pending);
                s.pending.clear();
                HoldWorkers held;
                s.pending = send(
                    s,
                    "apply",
                    {{"expected", s.snapshot.value("identity")},
                     {"proposalId", s.proposal.value("proposalId")}}
                );
                require(editor.stateBusy() && pipeline.busy(), "apply did not reserve the owner");
                rejected(s, send(s, "cancel", {{"requestId", s.pending}}), "cancel_unsupported");
                rejected(s, send(s, "snapshot"), "busy");
                const auto variant = editor.activeVariantId();
                editor.setExposureStops(3);
                editor.createVariant("Must not appear");
                editor.closePhoto();
                pipeline.complete();
                pipeline.cancel();
                require(
                    editor.active() && editor.exposureStops() == 0.0
                        && editor.activeVariantId() == variant,
                    "a human mutation bypassed the held apply reservation"
                );
                ++s.step;
                break;
            }
            case 6:
                s.receipt = ok(s, s.pending);
                s.pending.clear();
                ++s.step;
                break;
            case 7:
                require(
                    editor.exposureStops() == 1.0
                        && editor.durableWorkingCommitId()
                               == s.receipt.value("commitId").toString(),
                    "owner did not install its exact commit receipt"
                );
                capture(*window, root, "01-tool-applied.png");
                click(*window, "pipelineUndoButton");
                ++s.step;
                break;
            case 8:
                require(
                    editor.exposureStops() == 0.0,
                    "one actual Undo did not restore pre-tool exposure"
                );
                capture(*window, root, "02-tool-undone.png");
                click(*window, "pipelineRedoButton");
                ++s.step;
                break;
            case 9:
                require(editor.exposureStops() == 1.0, "actual Redo did not restore tool exposure");
                capture(*window, root, "03-tool-redone.png");
                require(
                    pipeline.configureExport(
                        QUrl::fromLocalFile(root.absolutePath()),
                        {{"format", "png"}}
                    ),
                    "prepare GUI export destination"
                );
                s.snapshot = ok(s, send(s, "snapshot"));
                {
                    HoldWorkers held;
                    s.pending = send(
                        s,
                        "export",
                        {{"expected", s.snapshot.value("identity")},
                         {"outputPath", root.filePath("owner-export.png")}}
                    );
                    rejected(
                        s,
                        send(s, "cancel", {{"requestId", s.pending}}),
                        "cancel_unsupported"
                    );
                    require(
                        pipeline.busy() && !editor.stateBusy(),
                        "export must serialize consumers while permitting human edits"
                    );
                    pipeline.complete();
                    require(
                        !pipeline.exportPending() && editor.active(),
                        "GUI started a second global queue consumer"
                    );
                    editor.setExposureStops(0.25);
                    require(
                        editor.exposureStops() == 0.25,
                        "export unnecessarily locked human edits"
                    );
                }
                ++s.step;
                break;
            case 10:
                s.export_receipt = ok(s, s.pending);
                s.pending.clear();
                require(
                    s.export_receipt.value("artifact").toObject().value("commitId")
                        == s.snapshot.value("identity").toObject().value("workingCommitId"),
                    "export drifted to later human working head"
                );
                ++s.step;
                break;
            case 11:
                require(
                    editor.exposureStops() == 0.25
                        && editor.durableWorkingCommitId()
                               != s.snapshot.value("identity")
                                      .toObject()
                                      .value("workingCommitId")
                                      .toString(),
                    "concurrent human edit was not retained"
                );
                ok(s, send(s, "shutdown"));
                require(
                    editor.stateBusy() && pipeline.busy(),
                    "draining shutdown did not reserve clean draft"
                );
                editor.setExposureStops(3);
                editor.undo();
                pipeline.cancel();
                require(
                    editor.exposureStops() == 0.25,
                    "new draft entered while shutdown was draining"
                );
                record("passed");
                timer->stop();
                QCoreApplication::exit(0);
                break;
            }
        } catch (const std::exception& error) {
            record("failed", QString::fromUtf8(error.what()));
            timer->stop();
            QCoreApplication::exit(3);
        }
    });
    timer->start();
}
