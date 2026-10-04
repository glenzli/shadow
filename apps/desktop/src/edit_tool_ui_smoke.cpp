#include "edit_tool_ui_smoke.hpp"
#include "edit_controller.hpp"
#include "edit_tool_controller.hpp"
#include "pipeline_run_controller.hpp"
#include "ui_preferences.hpp"
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
#include <QSemaphore>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QTest>
#endif

namespace {
struct State {
    std::shared_ptr<EditToolController> tools;
    QMap<QString, QJsonObject> replies;
    QJsonObject snapshot, proposal, receipt, export_receipt, retry_snapshot, retry_export_receipt;
    QJsonObject observation_snapshot, handoff_proposal, handoff_receipt, handoff_export;
    double preview_export_mean_error = 0;
    int preview_export_max_error = 0;
    QString pending, preview_path;
    bool cancel_checked = false, human_handoff_checked = false, multiple_nodes_ready = false;
    bool export_recovery_checked = false;
    int step = 0, requests = 0, ticks = 0, recovery_stage = 0, history_recovery_stage = 0;
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
void requireStatus(
    QQuickWindow& window,
    const PipelineRunController& pipeline,
    const QString& expected
) {
    auto* label = window.findChild<QQuickItem*>("pipelineStatusLabel");
    require(
        label && label->isVisible() && label->property("text").toString() == expected
            && pipeline.statusText() == expected,
        "packaged status label did not present the current localized tool activity"
    );
}
void requireLanguages(
    QQuickWindow& window,
    const PipelineRunController& pipeline,
    const QString& english,
    const QString& chinese
) {
    auto* preferences =
        qobject_cast<UiPreferences*>(window.property("preferences").value<QObject*>());
    require(preferences != nullptr, "isolated session preferences unavailable");
    const auto prior = preferences->languageMode();
    preferences->setLanguageMode("en");
    requireStatus(window, pipeline, english);
    preferences->setLanguageMode("zh_CN");
    requireStatus(window, pipeline, chinese);
    preferences->setLanguageMode("en");
    requireStatus(window, pipeline, english);
    preferences->setLanguageMode(prior);
}
void occupyOutput(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create publication collision");
    require(file.write("keep-existing") == 13, "write publication collision sentinel");
}
void requireOccupied(const QString& path) {
    QFile file(path);
    require(
        file.open(QIODevice::ReadOnly) && file.readAll() == "keep-existing",
        "publication overwrote an existing file"
    );
}
void capture(QQuickWindow& window, const QDir& root, const QString& name) {
    require(
        window.grabWindow().save(root.filePath(name)),
        "could not capture this application window"
    );
}
struct HoldWorkers {
    struct Gate {
        QSemaphore entered, release;
    };
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    HoldWorkers() {
        // reserveThread() is accounting, not a barrier: Qt may still run a
        // queued task. Park both real workers before creating publication races.
        for (int index = 0; index < 2; ++index)
            QThreadPool::globalInstance()->start([gate = gate] {
                gate->entered.release();
                gate->release.acquire();
            });
        if (!gate->entered.tryAcquire(2, 5000)) {
            gate->release.release(2);
            throw std::runtime_error("could not park the two test workers");
        }
    }
    ~HoldWorkers() {
        gate->release.release(2);
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
                file.write(
                    QJsonDocument(
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
                            {"retryExportReceipt", state->retry_export_receipt},
                            {"handoffReceipt", state->handoff_receipt},
                            {"handoffExport", state->handoff_export},
                            {"previewExportMeanByteError", state->preview_export_mean_error},
                            {"previewExportMaxByteError", state->preview_export_max_error},
                            {"platform", QGuiApplication::platformName()},
                            {"evidence",
                             QJsonArray{
                                 "snapshot_preserves_dirty_gesture",
                                 "held_preview_cancel_no_artifact",
                                 "held_apply_export_cancel_unsupported",
                                 "same_head_variant_rejected",
                                 "held_apply_blocks_close_and_edits",
                                 "actual_pointer_cross_node_atomic_composition_undo_redo",
                                 "human_handoff_preserved_after_stale_proposal",
                                 "packaged_qml_tool_status_en_zh_en",
                                 "terminal_feedback_en_zh_en",
                                 "repeated_snapshot_preserves_reviewed_proposal",
                                 "failed_and_cancelled_replacement_preserves_reviewed_proposal",
                                 "export_failure_retry_preserves_existing_file",
                                 "terminal_feedback_survives_observation_clears_on_human_edit",
                                 "enabled_only_undo_redo_clears_terminal_feedback",
                                 "export_blocks_second_runner",
                                 "human_edit_during_pinned_export",
                                 "shutdown_reserves_clean_draft",
                                 "external_returning_rejects_tool_admission",
                                 "terminal_seal_blocks_owner_mutation_and_undo",
                                 "current_observation_failure_cancel_retry_en_zh_en",
                                 "reviewed_proposal_survives_observation_failure_cancel_retry",
                                 "human_draft_invalidates_pending_observation",
                                 "ai_human_saved_ai_observe_apply_export",
                                 "reviewed_preview_matches_applied_current_rendition",
                                 "handoff_preview_export_same_size_pixel_tolerance",
                                 "resumed_ai_undo_redo_preserves_human_state"
                             }}
                        }
                    ).toJson()
                );
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
            const auto preview = [&](const double exposure = 1.0) {
                const auto id = s.snapshot.value("identity").toObject();
                const auto nodes = s.snapshot.value("nodes").toArray();
                const auto node = nodes.first().toObject();
                QJsonArray operations{
                    QJsonObject{
                        {"type", "set_exposure"},
                        {"nodeId", node.value("nodeId")},
                        {"stops", exposure}
                    },
                    QJsonObject{
                        {"type", "set_contrast"},
                        {"nodeId", node.value("nodeId")},
                        {"factor", 1.2}
                    },
                    QJsonObject{
                        {"type", "set_saturation"},
                        {"nodeId", node.value("nodeId")},
                        {"factor", 0.75}
                    }
                };
                if (nodes.size() > 1)
                    operations.append(
                        QJsonObject{
                            {"type", "set_exposure"},
                            {"nodeId", nodes.at(1).toObject().value("nodeId")},
                            {"stops", -0.25}
                        }
                    );
                s.preview_path =
                    root.filePath(QStringLiteral("owner-preview-%1.jpg").arg(s.requests + 1));
                return send(
                    s,
                    "preview",
                    {{"expected", id}, {"operations", operations}, {"outputPath", s.preview_path}}
                );
            };
            const auto verifySecondNode = [&](const double exposure) {
                require(editor.gradeNodes().size() == 2, "composition changed the node topology");
                editor.selectGradeNode(1);
                require(
                    editor.exposureStops() == exposure,
                    "second node did not follow atomic history"
                );
                editor.selectGradeNode(0);
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
                if (!s.multiple_nodes_ready) {
                    editor.addGradeNode();
                    require(editor.gradeNodes().size() == 2, "could not create second Grade Node");
                    editor.selectGradeNode(0);
                    s.multiple_nodes_ready = true;
                    break; // Wait for the human node addition to render and autosave.
                }
                if (!s.cancel_checked) {
                    s.snapshot = ok(s, send(s, "snapshot"));
                    HoldWorkers held;
                    s.pending = preview();
                    requireStatus(
                        *window,
                        pipeline,
                        QCoreApplication::translate(
                            "EditToolController",
                            "Rendering proposed adjustments…"
                        )
                    );
                    const auto result = ok(s, send(s, "cancel", {{"requestId", s.pending}}));
                    require(result.value("accepted").toBool(), "held preview was not cancelled");
                    s.cancel_checked = true;
                    break;
                }
                rejected(s, s.pending, "cancelled");
                s.pending.clear();
                requireLanguages(
                    *window,
                    pipeline,
                    "Proposed preview cancelled.",
                    QString::fromUtf8("调整方案预览已取消。")
                );
                require(!QFile::exists(s.preview_path), "cancelled preview published an artifact");
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
                if (s.recovery_stage == 1) {
                    rejected(s, s.pending, "preview_failed");
                    s.pending.clear();
                    requireLanguages(
                        *window,
                        pipeline,
                        "Could not preview the proposed adjustments. Try again.",
                        QString::fromUtf8("调整方案预览失败，请重试。")
                    );
                    requireOccupied(s.preview_path);
                    capture(*window, root, "04-preview-failed.png");
                    require(
                        ok(s, send(s, "snapshot")).value("identity")
                            == s.snapshot.value("identity"),
                        "failed replacement changed the reviewed snapshot"
                    );
                    HoldWorkers held;
                    s.pending = preview(3.0);
                    requireStatus(
                        *window,
                        pipeline,
                        QCoreApplication::translate(
                            "EditToolController",
                            "Rendering proposed adjustments…"
                        )
                    );
                    require(
                        ok(s, send(s, "cancel", {{"requestId", s.pending}}))
                            .value("accepted")
                            .toBool(),
                        "replacement cancellation not admitted"
                    );
                    s.recovery_stage = 2;
                    break;
                }
                if (s.recovery_stage == 2) {
                    rejected(s, s.pending, "cancelled");
                    s.pending.clear();
                    requireLanguages(
                        *window,
                        pipeline,
                        "Proposed preview cancelled.",
                        QString::fromUtf8("调整方案预览已取消。")
                    );
                    require(
                        !QFile::exists(s.preview_path),
                        "cancelled replacement published bytes"
                    );
                    require(
                        ok(s, send(s, "snapshot")).value("identity")
                            == s.snapshot.value("identity"),
                        "cancelled replacement consumed the reviewed snapshot"
                    );
                    s.recovery_stage = 3;
                } else {
                    s.proposal = ok(s, s.pending);
                    s.pending.clear();
                }
                if (!s.human_handoff_checked) {
                    editor.beginParameterEdit("white_balance_temperature");
                    editor.setWhiteBalanceTemperature(0.2);
                    rejected(
                        s,
                        send(
                            s,
                            "apply",
                            {{"expected", s.snapshot.value("identity")},
                             {"proposalId", s.proposal.value("proposalId")}}
                        ),
                        "stale_snapshot"
                    );
                    editor.endParameterEdit("white_balance_temperature");
                    require(
                        editor.whiteBalanceTemperature() == 0.2 && editor.exposureStops() == 0.0,
                        "stale proposal overwrote the human handoff"
                    );
                    s.human_handoff_checked = true;
                    s.step = 4;
                    break;
                }
                require(
                    s.proposal.value("changes").toArray().size() == 4,
                    "composed preview did not describe all edits"
                );
                if (s.recovery_stage == 0) {
                    HoldWorkers held;
                    s.pending = preview(2.0);
                    occupyOutput(
                        s.preview_path
                    ); // The path was valid at admission; publication must fail safely.
                    s.recovery_stage = 1;
                    break;
                }
                HoldWorkers held;
                s.pending = send(
                    s,
                    "apply",
                    {{"expected", s.snapshot.value("identity")},
                     {"proposalId", s.proposal.value("proposalId")}}
                );
                require(editor.stateBusy() && pipeline.busy(), "apply did not reserve the owner");
                requireLanguages(
                    *window,
                    pipeline,
                    "Applying proposed adjustments…",
                    QString::fromUtf8("正在应用调整方案…")
                );
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
                    editor.exposureStops() == 1.0 && editor.contrastFactor() == 1.2
                        && editor.saturationFactor() == 0.75
                        && editor.whiteBalanceTemperature() == 0.2
                        && editor.durableWorkingCommitId()
                               == s.receipt.value("commitId").toString(),
                    "owner did not install its exact commit receipt"
                );
                verifySecondNode(-0.25);
                capture(*window, root, "01-tool-applied.png");
                click(*window, "pipelineUndoButton");
                ++s.step;
                break;
            case 8:
                require(
                    editor.exposureStops() == 0.0 && editor.contrastFactor() == 1.0
                        && editor.saturationFactor() == 1.0
                        && editor.whiteBalanceTemperature() == 0.2,
                    "one actual Undo did not restore all pre-tool values and retain the human edit"
                );
                verifySecondNode(0.0);
                capture(*window, root, "02-tool-undone.png");
                click(*window, "pipelineRedoButton");
                ++s.step;
                break;
            case 9:
                require(
                    editor.exposureStops() == 1.0 && editor.contrastFactor() == 1.2
                        && editor.saturationFactor() == 0.75
                        && editor.whiteBalanceTemperature() == 0.2,
                    "actual Redo did not restore the complete proposal"
                );
                verifySecondNode(-0.25);
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
                    requireStatus(
                        *window,
                        pipeline,
                        QCoreApplication::translate(
                            "EditToolController",
                            "Exporting the captured adjustments…"
                        )
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
                if (!s.export_recovery_checked) {
                    s.retry_snapshot = ok(s, send(s, "snapshot"));
                    HoldWorkers held;
                    const auto output = root.filePath("owner-export-failed.png");
                    s.pending = send(
                        s,
                        "export",
                        {{"expected", s.retry_snapshot.value("identity")}, {"outputPath", output}}
                    );
                    occupyOutput(output);
                    s.step = 12;
                    break;
                }
                s.observation_snapshot = ok(s, send(s, "snapshot"));
                {
                    const auto second =
                        s.observation_snapshot.value("nodes").toArray().at(1).toObject();
                    s.pending = send(
                        s,
                        "preview",
                        {{"expected", s.observation_snapshot.value("identity")},
                         {"operation",
                          QJsonObject{
                              {"type", "set_exposure"},
                              {"nodeId", second.value("nodeId")},
                              {"stops", -0.5}
                          }},
                         {"outputPath", root.filePath("handoff-proposal.jpg")}}
                    );
                }
                s.step = 19;
                break;
            case 12:
                rejected(s, s.pending, "export_failed");
                s.pending.clear();
                requireLanguages(
                    *window,
                    pipeline,
                    "Could not export the captured adjustments. Try again.",
                    QString::fromUtf8("已确认的调整导出失败，请重试。")
                );
                requireOccupied(root.filePath("owner-export-failed.png"));
                capture(*window, root, "05-export-failed.png");
                ok(s, send(s, "snapshot"));
                editor.selectGradeNode(1);
                editor.selectGradeNode(0);
                requireStatus(
                    *window,
                    pipeline,
                    QCoreApplication::translate(
                        "EditToolController",
                        "Could not export the captured adjustments. Try again."
                    )
                );
                require(editor.gradeNodeEnabled(), "human structural edit fixture starts enabled");
                editor.setGradeNodeEnabled(false);
                require(
                    pipeline.statusText()
                        != QCoreApplication::translate(
                            "EditToolController",
                            "Could not export the captured adjustments. Try again."
                        ),
                    "terminal feedback masked a new human edit"
                );
                s.step = 14; // Wait for the new human edit to autosave before retrying.
                break;
            case 14:
                s.retry_snapshot = ok(s, send(s, "snapshot"));
                if (s.history_recovery_stage < 2) {
                    HoldWorkers held;
                    const auto output =
                        root.filePath(QStringLiteral("owner-export-history-failed-%1.png")
                                          .arg(s.history_recovery_stage));
                    s.pending = send(
                        s,
                        "export",
                        {{"expected", s.retry_snapshot.value("identity")}, {"outputPath", output}}
                    );
                    occupyOutput(output);
                    s.step = 15;
                    break;
                }
                s.pending = send(
                    s,
                    "export",
                    {{"expected", s.retry_snapshot.value("identity")},
                     {"outputPath", root.filePath("owner-export-retry.png")}}
                );
                requireStatus(
                    *window,
                    pipeline,
                    QCoreApplication::translate(
                        "EditToolController",
                        "Exporting the captured adjustments…"
                    )
                );
                s.step = 13;
                break;
            case 15: {
                rejected(s, s.pending, "export_failed");
                s.pending.clear();
                const auto failure = QCoreApplication::translate(
                    "EditToolController",
                    "Could not export the captured adjustments. Try again."
                );
                requireStatus(*window, pipeline, failure);
                requireOccupied(root.filePath(QStringLiteral("owner-export-history-failed-%1.png")
                                                  .arg(s.history_recovery_stage)));
                const bool undo = s.history_recovery_stage == 0;
                click(*window, undo ? "pipelineUndoButton" : "pipelineRedoButton");
                require(
                    editor.gradeNodeEnabled() == undo,
                    "actual Undo/Redo did not restore the enabled-only edit"
                );
                require(
                    pipeline.statusText() != failure,
                    "terminal feedback masked an enabled-only Undo/Redo"
                );
                ++s.history_recovery_stage;
                s.step = 14; // Wait for history autosave before the next captured export.
                break;
            }
            case 13:
                s.retry_export_receipt = ok(s, s.pending);
                s.pending.clear();
                require(
                    s.retry_export_receipt.value("artifact").toObject().value("commitId")
                        == s.retry_snapshot.value("identity").toObject().value("workingCommitId"),
                    "retry exported the failed job instead of its captured commit"
                );
                require(
                    !QImage(root.filePath("owner-export-retry.png")).isNull(),
                    "retry did not publish a decodable PNG"
                );
                require(
                    pipeline.statusText()
                        != QCoreApplication::translate(
                            "EditToolController",
                            "Could not export the captured adjustments. Try again."
                        ),
                    "successful retry retained old failure feedback"
                );
                s.export_recovery_checked = true;
                s.step = 11;
                break;
            case 16:
                rejected(s, s.pending, "preview_failed");
                s.pending.clear();
                requireOccupied(root.filePath("current-failed.jpg"));
                requireLanguages(
                    *window,
                    pipeline,
                    "Could not preview the current edits. Try again.",
                    QString::fromUtf8("无法预览当前编辑，请重试。")
                );
                require(
                    ok(s, send(s, "snapshot")).value("identity")
                        == s.observation_snapshot.value("identity"),
                    "failed observation changed the saved human identity"
                );
                {
                    HoldWorkers held;
                    s.pending = send(
                        s,
                        "render_current",
                        {{"expected", s.observation_snapshot.value("identity")},
                         {"outputPath", root.filePath("current-cancelled.jpg")}}
                    );
                    require(
                        ok(s, send(s, "cancel", {{"requestId", s.pending}}))
                            .value("accepted")
                            .toBool(),
                        "held current observation did not accept cancellation"
                    );
                }
                s.step = 17;
                break;
            case 17:
                rejected(s, s.pending, "cancelled");
                s.pending.clear();
                require(
                    !QFile::exists(root.filePath("current-cancelled.jpg")),
                    "cancelled current observation published an artifact"
                );
                requireLanguages(
                    *window,
                    pipeline,
                    "Current edits preview cancelled.",
                    QString::fromUtf8("当前编辑预览已取消。")
                );
                s.pending = send(
                    s,
                    "render_current",
                    {{"expected", s.observation_snapshot.value("identity")},
                     {"outputPath", root.filePath("current-human-saved.jpg")}}
                );
                s.step = 18;
                break;
            case 18: {
                const auto observed = ok(s, s.pending);
                s.pending.clear();
                require(
                    observed.value("current").toBool() && !observed.contains("proposalId")
                        && observed.value("expected") == s.observation_snapshot.value("identity"),
                    "observation was mislabeled as a proposal or a different snapshot"
                );
                require(
                    !QImage(root.filePath("current-human-saved.jpg")).isNull(),
                    "current human image is not decodable"
                );
                const auto fresh = ok(s, send(s, "snapshot"));
                require(
                    fresh.value("identity") == s.observation_snapshot.value("identity")
                        && fresh.value("nodes") == s.observation_snapshot.value("nodes"),
                    "observation altered human state"
                );
                // Apply the exact reviewed proposal retained through the
                // asynchronous observation failure, accepted cancel and retry.
                s.pending = send(
                    s,
                    "apply",
                    {{"expected", s.observation_snapshot.value("identity")},
                     {"proposalId", s.handoff_proposal.value("proposalId")}}
                );
                s.step = 20;
                break;
            }
            case 19:
                s.handoff_proposal = ok(s, s.pending);
                s.pending.clear();
                require(
                    s.handoff_proposal.value("current").toBool(),
                    "fresh handoff proposal is stale"
                );
                {
                    HoldWorkers held;
                    const auto output = root.filePath("current-failed.jpg");
                    s.pending = send(
                        s,
                        "render_current",
                        {{"expected", s.observation_snapshot.value("identity")},
                         {"outputPath", output}}
                    );
                    requireLanguages(
                        *window,
                        pipeline,
                        "Rendering current edits…",
                        QString::fromUtf8("正在渲染当前编辑…")
                    );
                    occupyOutput(output);
                }
                s.step = 16;
                break;
            case 20:
                s.handoff_receipt = ok(s, s.pending);
                s.pending.clear();
                s.step = 21; // Wait for the committed owner's display/autosave work.
                break;
            case 21: {
                verifySecondNode(-0.5);
                require(
                    editor.exposureStops() == 0.25 && editor.whiteBalanceTemperature() == 0.2
                        && !editor.gradeNodeEnabled(),
                    "resumed AI overwrote the saved human changes"
                );
                const auto fresh = ok(s, send(s, "snapshot"));
                require(
                    fresh.value("nodes").toArray().first()
                        == s.observation_snapshot.value("nodes").toArray().first(),
                    "resumed AI changed the unaddressed human node"
                );
                require(
                    fresh.value("identity").toObject().value("workingCommitId")
                        == s.handoff_receipt.value("commitId"),
                    "resumed AI did not install its exact receipt"
                );
                s.observation_snapshot = fresh;
                s.pending = send(
                    s,
                    "render_current",
                    {{"expected", fresh.value("identity")},
                     {"outputPath", root.filePath("handoff-applied.jpg")}}
                );
                s.step = 22;
                break;
            }
            case 22: {
                const auto current = ok(s, s.pending);
                s.pending.clear();
                require(
                    current.value("current").toBool()
                        && current.value("artifact").toObject().value("sha256")
                               == s.handoff_proposal.value("artifact").toObject().value("sha256"),
                    "applying a reviewed proposal changed its current rendition"
                );
                s.pending = send(
                    s,
                    "export",
                    {{"expected", s.observation_snapshot.value("identity")},
                     {"outputPath", root.filePath("handoff-export.png")}}
                );
                s.step = 23;
                break;
            }
            case 23: {
                s.handoff_export = ok(s, s.pending);
                s.pending.clear();
                require(
                    s.handoff_export.value("artifact").toObject().value("commitId")
                        == s.handoff_receipt.value("commitId"),
                    "handoff export used another committed version"
                );
                const QImage preview_image(root.filePath("handoff-applied.jpg"));
                const QImage export_image(root.filePath("handoff-export.png"));
                require(
                    !preview_image.isNull() && !export_image.isNull()
                        && preview_image.size() == export_image.size(),
                    "same-size synthetic handoff images unavailable"
                );
                double sum = 0;
                for (int y = 0; y < preview_image.height(); ++y) {
                    for (int x = 0; x < preview_image.width(); ++x) {
                        const auto a = preview_image.pixel(x, y), b = export_image.pixel(x, y);
                        for (int shift : {0, 8, 16}) {
                            const int delta =
                                std::abs(int((a >> shift) & 255) - int((b >> shift) & 255));
                            sum += delta;
                            s.preview_export_max_error =
                                std::max(s.preview_export_max_error, delta);
                        }
                    }
                }
                s.preview_export_mean_error =
                    sum / (preview_image.width() * preview_image.height() * 3.0);
                require(
                    s.preview_export_mean_error <= 2.0 && s.preview_export_max_error <= 12,
                    "same-size synthetic preview/export diverged beyond JPEG tolerance"
                );
                click(*window, "pipelineUndoButton");
                s.step = 24;
                break;
            }
            case 24:
                verifySecondNode(-0.25);
                require(
                    editor.exposureStops() == 0.25 && editor.whiteBalanceTemperature() == 0.2
                        && !editor.gradeNodeEnabled(),
                    "resumed AI Undo erased human changes"
                );
                click(*window, "pipelineRedoButton");
                s.step = 25;
                break;
            case 25:
                verifySecondNode(-0.5);
                require(
                    editor.exposureStops() == 0.25 && editor.whiteBalanceTemperature() == 0.2
                        && !editor.gradeNodeEnabled(),
                    "resumed AI Redo erased human changes"
                );
                // The preserved first human node is disabled; edit the enabled
                // second node, as the real controls require.
                editor.selectGradeNode(1);
                require(editor.gradeNodeEnabled(), "human draft target must be enabled");
                s.observation_snapshot = ok(s, send(s, "snapshot"));
                {
                    HoldWorkers held;
                    s.pending = send(
                        s,
                        "render_current",
                        {{"expected", s.observation_snapshot.value("identity")},
                         {"outputPath", root.filePath("current-before-human.jpg")}}
                    );
                    editor.setWhiteBalanceTemperature(0.3);
                    require(
                        editor.whiteBalanceTemperature() == 0.3,
                        "pending observation blocked the human draft"
                    );
                }
                s.step = 27;
                break;
            case 27:
                rejected(s, s.pending, "cancelled");
                s.pending.clear();
                require(
                    !QFile::exists(root.filePath("current-before-human.jpg"))
                        && editor.whiteBalanceTemperature() == 0.3,
                    "invalidated observation published or overwrote the new human draft"
                );
                s.step = 28; // Wait for the new human draft to save.
                break;
            case 28:
                rejected(
                    s,
                    send(
                        s,
                        "render_current",
                        {{"expected", s.observation_snapshot.value("identity")},
                         {"outputPath", root.filePath("current-stale-human.jpg")}}
                    ),
                    "stale_snapshot"
                );
                s.observation_snapshot = ok(s, send(s, "snapshot"));
                s.pending = send(
                    s,
                    "render_current",
                    {{"expected", s.observation_snapshot.value("identity")},
                     {"outputPath", root.filePath("current-after-human.jpg")}}
                );
                s.step = 29;
                break;
            case 29: {
                const auto observed = ok(s, s.pending);
                s.pending.clear();
                require(
                    observed.value("current").toBool()
                        && observed.value("expected") == s.observation_snapshot.value("identity")
                        && editor.whiteBalanceTemperature() == 0.3,
                    "fresh observation failed to retain the saved human draft"
                );
                editor.selectGradeNode(0);
                require(
                    editor.whiteBalanceTemperature() == 0.2 && !editor.gradeNodeEnabled(),
                    "observation invalidation changed the preceding human node"
                );
                s.step = 26;
                break;
            }
            case 26:
                s.tools->setStageReturning(true);
                rejected(s, send(s, "snapshot"), "session_finishing");
                s.tools->setStageReturning(false);
                {
                    EditToolController terminal_guard(editor, backend, source);
                    terminal_guard.sealSession();
                    require(editor.stateBusy(), "terminal drain did not reserve the owner");
                    const auto nodes = editor.gradeNodes();
                    editor.setExposureStops(3);
                    require(
                        editor.exposureStops() == 0.25 && editor.gradeNodes() == nodes,
                        "terminal drain admitted a parameter mutation"
                    );
                    editor.undo();
                    require(
                        editor.exposureStops() == 0.25 && editor.gradeNodes() == nodes,
                        "terminal drain admitted Undo"
                    );
                }
                ok(s, send(s, "shutdown"));
                require(
                    editor.stateBusy() && pipeline.busy(),
                    "draining shutdown did not reserve clean draft"
                );
                {
                    const auto nodes = editor.gradeNodes();
                    editor.setExposureStops(3);
                    require(
                        editor.exposureStops() == 0.25 && editor.gradeNodes() == nodes,
                        "new draft entered while shutdown was draining"
                    );
                    editor.undo();
                    require(
                        editor.exposureStops() == 0.25 && editor.gradeNodes() == nodes,
                        "Undo entered while shutdown was draining"
                    );
                    pipeline.cancel();
                }
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
