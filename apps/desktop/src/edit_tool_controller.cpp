#include "edit_tool_controller.hpp"
#include "backend/export_backend.hpp"
#include "backend/export_output_file.hpp"
#include "backend/export_settings_codec.hpp"
#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"
#include "edit_targeted_curve_controller.hpp"
#include "export_task_runner.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <stdexcept>

namespace {
QString token() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString newOutput(const QString& path, const bool preview) {
    const QFileInfo info(path);
    const auto suffix = info.suffix().toLower();
    if (!info.isAbsolute() || info.exists() || info.isSymLink() || !info.dir().exists()
        || (preview ? suffix != "jpg" && suffix != "jpeg" : suffix != "png"))
        throw std::invalid_argument(
            "output must be a new absolute JPEG preview or PNG export in an existing directory"
        );
    return QDir(info.dir().canonicalPath()).filePath(info.fileName());
}
QJsonObject artifact(
    const QString& path,
    const QString& format,
    const QSize& size,
    const QByteArray& digest,
    const qint64 bytes
) {
    return {
        {"path", path},
        {"format", format},
        {"width", size.width()},
        {"height", size.height()},
        {"sha256", QString::fromLatin1(digest.toHex())},
        {"byteLength", QString::number(bytes)}
    };
}
bool editable(
    const BackendGradeNode& node,
    const EditToolProtocol::EditKind kind = EditToolProtocol::EditKind::Exposure
) {
    if (!node.enabled || !node.shared_layer_id.isEmpty())
        return false;
    using enum EditToolProtocol::EditKind;
    switch (kind) {
    case Exposure:
        return !node.exposure_render_op_id.isEmpty();
    case Contrast:
        return !node.contrast_render_op_id.isEmpty();
    case Saturation:
        return !node.saturation_render_op_id.isEmpty();
    }
    return false;
}
double BackendBasicEditParameters::* parameter(const EditToolProtocol::EditKind kind) {
    using enum EditToolProtocol::EditKind;
    switch (kind) {
    case Exposure:
        return &BackendBasicEditParameters::exposure_stops;
    case Contrast:
        return &BackendBasicEditParameters::contrast_factor;
    case Saturation:
        return &BackendBasicEditParameters::saturation_factor;
    }
    throw std::invalid_argument("unsupported adjustment");
}
} // namespace

EditToolController::EditToolController(
    EditController& owner,
    std::shared_ptr<DesktopBackend> backend,
    QString admitted_source
) :
    owner_(owner), backend_(std::move(backend)), session_id_(token()),
    admitted_source_(std::move(admitted_source)) {
    connect(
        &watcher_,
        &QFutureWatcher<EditToolTaskResult>::finished,
        this,
        &EditToolController::finishTask
    );
    connect(&owner_, &EditController::parametersChanged, this, &EditToolController::invalidate);
    connect(&owner_, &EditController::photoVariantsChanged, this, &EditToolController::invalidate);
    connect(&owner_, &EditController::activeChanged, this, &EditToolController::invalidate);
}
EditToolController::~EditToolController() {
    disconnectClient();
    watcher_.waitForFinished();
    owner_.tool_commit_reserved_ = false;
    owner_.tool_operation_running_ = false;
    owner_.tool_activity_message_.clear();
}
void EditToolController::fail(const QString& id, const QString& code, const QString& message) {
    emit reply(EditToolProtocol::failure(id, code, message));
}
void EditToolController::invalidate() {
    ++owner_epoch_;
    if (preview_stage_) {
        int expected = 0;
        if (preview_stage_->compare_exchange_strong(expected, 1))
            (void)backend_->cancelEditPreviewRequest(preview_token_);
    }
}
void EditToolController::disconnectClient() {
    if (preview_stage_) {
        int expected = 0;
        if (preview_stage_->compare_exchange_strong(expected, 1))
            (void)backend_->cancelEditPreviewRequest(preview_token_);
    }
    // Detach on EOF. Keep the real window and any human edits alive.
}
bool EditToolController::gestureActive() const {
    return !owner_.active_parameter_gestures_.isEmpty() || owner_.liquify_live_before_.has_value()
           || (owner_.paint_controller_ && owner_.paint_controller_->strokeActive())
           || (owner_.targeted_curve_controller_ && owner_.targeted_curve_controller_->dragging());
}
bool EditToolController::current() const {
    if (!snapshot_ || !owner_.active_ || owner_.source_path_ != admitted_source_)
        return false;
    const auto& capture = *snapshot_;
    const auto& id = capture.identity;
    const QFileInfo source(capture.source);
    return source.isFile() && source.size() == capture.source_bytes
           && source.lastModified().toMSecsSinceEpoch() == capture.source_modified_ms
           && capture.owner_epoch == owner_epoch_
           && capture.photo_generation == owner_.photo_generation_
           && capture.working_revision == owner_.working_revision_
           && id.value("photoId").toString() == owner_.photo_id_
           && id.value("representationId").toString() == owner_.representation_id_
           && id.value("baseCommitId").toString() == owner_.base_commit_id_
           && id.value("workingCommitId").toString() == owner_.durable_working_commit_id_
           && id.value("activeVariantId").toString() == owner_.active_variant_id_
           && capture.stack == owner_.grade_stack_;
}
bool EditToolController::admit(const EditToolProtocol::Request& request) {
    if (!current() || request.expected != snapshot_->identity) {
        fail(
            request.id,
            "stale_snapshot",
            "capture a fresh snapshot; the editor or session changed"
        );
        return false;
    }
    if (owner_.dirty_ || owner_.autosavePending() || owner_.autosaveFailed() || gestureActive()) {
        fail(
            request.id,
            "draft_pending",
            "finish and save the human draft before using this snapshot"
        );
        return false;
    }
    if (owner_.busy() || owner_.interactionLocked()) {
        fail(
            request.id,
            "busy",
            "the editor owns an active operation; retry with a fresh snapshot"
        );
        return false;
    }
    return true;
}
void EditToolController::setAdmissionError(const QString& message) {
    admission_error_ = message;
}
void EditToolController::capture(const QString& id) {
    if (!owner_.active_ && !admission_error_.isEmpty()) {
        fail(id, "source_unavailable", admission_error_);
        return;
    }
    // A failed asynchronous history open retains its source identity; normal
    // finalizePhotoClose clears that identity before making the owner inactive.
    if (!owner_.active_ && !owner_.stateTaskRunning() && owner_.source_path_ == admitted_source_) {
        fail(id, "source_unavailable", owner_.statusText());
        return;
    }
    if (!owner_.active_ || owner_.stateTaskRunning() || owner_.source_path_ != admitted_source_) {
        fail(id, "not_ready", "the explicitly admitted photo is not ready in this editor");
        return;
    }
    Snapshot captured;
    captured.identity = {
        {"sessionId", session_id_},
        {"snapshotId", token()},
        {"photoId", owner_.photo_id_},
        {"representationId", owner_.representation_id_},
        {"baseCommitId", owner_.base_commit_id_},
        {"workingCommitId", owner_.durable_working_commit_id_},
        {"activeVariantId", owner_.active_variant_id_},
        {"draftRevision", QString::number(owner_.working_revision_)}
    };
    captured.stack = owner_.grade_stack_;
    captured.source = owner_.source_path_;
    const QFileInfo source(captured.source);
    captured.source_bytes = source.size();
    captured.source_modified_ms = source.lastModified().toMSecsSinceEpoch();
    captured.photo_generation = owner_.photo_generation_;
    captured.working_revision = owner_.working_revision_;
    captured.owner_epoch = owner_epoch_;
    snapshot_ = std::move(captured);
    proposal_id_.clear();
    candidate_ = {};
    candidate_changes_ = {};
    QJsonArray nodes;
    for (const auto& node : snapshot_->stack.grade_nodes) {
        QJsonArray available;
        for (const auto& spec : EditToolProtocol::editSpecs)
            if (editable(node, spec.kind))
                available.append(spec.type);
        nodes.append(
            QJsonObject{
                {"nodeId", node.grade_node_id},
                {"label", node.label},
                {"exposureStops", node.basic.exposure_stops},
                {"contrastFactor", node.basic.contrast_factor},
                {"saturationFactor", node.basic.saturation_factor},
                {"availableEdits", available},
                {"editable", editable(node)}
            }
        );
    }
    emit reply(
        EditToolProtocol::success(
            id,
            {{"identity", snapshot_->identity},
             {"nodes", nodes},
             {"sourceFingerprint",
              QJsonObject{
                  {"byteLength", QString::number(snapshot_->source_bytes)},
                  {"modifiedAtMs", QString::number(snapshot_->source_modified_ms)},
                  {"kind", "size_mtime_not_content_hash"}
              }},
             {"dirty", owner_.dirty_},
             {"autosavePending", owner_.autosavePending()},
             {"autosaveFailed", owner_.autosaveFailed()},
             {"gestureActive", gestureActive()},
             {"busy", owner_.busy() || owner_.interactionLocked()},
             {"canUndo", owner_.canUndo()}}
        )
    );
}
void EditToolController::receive(const QByteArray& line) {
    const auto parsed = EditToolProtocol::parse(line);
    if (!parsed.request) {
        fail(parsed.id, "invalid_request", parsed.error);
        return;
    }
    const auto& request = *parsed.request;
    if (seen_ids_.contains(request.id)) {
        fail(request.id, "duplicate_request", "use a new correlation id");
        return;
    }
    seen_ids_.insert(request.id);
    id_order_.enqueue(request.id);
    if (id_order_.size() > 256)
        seen_ids_.remove(id_order_.dequeue());
    using enum EditToolProtocol::Command;
    if (request.command == Discover) {
        emit reply(EditToolProtocol::success(request.id, EditToolProtocol::discovery()));
        return;
    }
    if (request.command == Cancel) {
        if (!task_ || task_->id != request.request_id) {
            fail(request.id, "not_running", "request is not running");
            return;
        }
        if (task_->command != Preview) {
            fail(request.id, "cancel_unsupported", "apply and export cannot be cancelled");
            return;
        }
        int expected = 0;
        const bool accepted =
            preview_stage_ && preview_stage_->compare_exchange_strong(expected, 1);
        if (accepted)
            (void)backend_->cancelEditPreviewRequest(preview_token_);
        emit reply(
            EditToolProtocol::success(
                request.id,
                {{"accepted", accepted},
                 {"reason", accepted ? "cancel_requested" : "publication_started_or_finished"}}
            )
        );
        return;
    }
    if (task_) {
        fail(request.id, "busy", "one tool operation is already running");
        return;
    }
    if (request.command == Snapshot) {
        capture(request.id);
        return;
    }
    if (request.command == Shutdown) {
        if (owner_.dirty_ || owner_.autosavePending() || gestureActive() || owner_.busy()
            || owner_.stateBusy()) {
            fail(
                request.id,
                "draft_pending",
                "editor work is still active; shutdown did not discard it"
            );
            return;
        }
        reserveCommit(true);
        operationRunning(true); // Hold the clean state through stdout backpressure and drain.
        emit reply(EditToolProtocol::success(request.id, {{"closing", true}}));
        emit shutdownRequested();
        return;
    }
    if (!admit(request))
        return;
    try {
        switch (request.command) {
        case Preview:
            preview(request);
            break;
        case Apply:
            apply(request);
            break;
        case Export:
            exportFile(request);
            break;
        default:
            break;
        }
    } catch (const std::exception& error) {
        fail(request.id, "invalid_operation", QString::fromUtf8(error.what()));
    }
}
void EditToolController::preview(const EditToolProtocol::Request& request) {
    auto stack = snapshot_->stack;
    QJsonArray changes;
    for (const auto& edit : request.edits) {
        auto found =
            std::find_if(stack.grade_nodes.begin(), stack.grade_nodes.end(), [&](const auto& node) {
                return node.grade_node_id == edit.node_id && editable(node, edit.kind);
            });
        if (found == stack.grade_nodes.end())
            throw std::invalid_argument(
                "node does not support this local adjustment in the captured snapshot"
            );
        auto& value = found->basic.*parameter(edit.kind);
        if (value != edit.value) {
            const auto& spec = EditToolProtocol::editSpecs.at(static_cast<size_t>(edit.kind));
            changes.append(
                QJsonObject{
                    {"type", spec.type},
                    {"nodeId", edit.node_id},
                    {"previousValue", value},
                    {"value", edit.value},
                    {"units", spec.units}
                }
            );
            value = edit.value;
        }
    }
    if (changes.isEmpty())
        throw std::invalid_argument("proposal would not change the captured adjustments");
    const auto output = newOutput(request.output_path, true);
    // Validate the whole candidate before admitting work or replacing the previous proposal.
    candidate_ = stack;
    candidate_changes_ = changes;
    candidate_node_ = request.edits.front().node_id;
    proposal_id_.clear();
    task_ = request;
    operationRunning(true);
    preview_stage_ = std::make_shared<std::atomic_int>(0);
    preview_token_ = backend_->beginEditPreviewRequest();
    watcher_.setFuture(
        QtConcurrent::run([backend = backend_,
                           capture = *snapshot_,
                           stack,
                           output,
                           stage = preview_stage_,
                           token = preview_token_] {
            EditToolTaskResult result;
            try {
                const auto preview = backend->renderAutoStartPreview(
                    capture.identity.value("photoId").toString(),
                    capture.source,
                    capture.identity.value("baseCommitId").toString(),
                    stack,
                    token,
                    {}
                );
                int expected = 0;
                if (preview.terminal == EditPreviewTerminal::Cancelled
                    || !stage->compare_exchange_strong(expected, 2)) {
                    result.code = "cancelled";
                    result.error = "preview cancelled before artifact publication";
                    return result;
                }
                ExportOutputFile file(output);
                if (file.device().write(preview.bytes) != preview.bytes.size())
                    throw std::runtime_error("could not stage complete preview JPEG");
                const auto bytes = file.commit();
                result.artifact = artifact(
                    output,
                    "jpeg",
                    QSize(static_cast<int>(preview.width), static_cast<int>(preview.height)),
                    QCryptographicHash::hash(preview.bytes, QCryptographicHash::Sha256),
                    static_cast<qint64>(bytes)
                );
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
                result.code = "preview_failed";
            }
            stage->store(3);
            return result;
        })
    );
}
void EditToolController::operationRunning(bool running) {
    owner_.tool_activity_message_.clear();
    if (running && task_) {
        using enum EditToolProtocol::Command;
        switch (task_->command) {
        case Preview:
            owner_.tool_activity_message_ = {
                "EditToolController",
                QT_TRANSLATE_NOOP("EditToolController", "Rendering proposed adjustments…")
            };
            break;
        case Apply:
            owner_.tool_activity_message_ = {
                "EditToolController",
                QT_TRANSLATE_NOOP("EditToolController", "Applying proposed adjustments…")
            };
            break;
        case Export:
            owner_.tool_activity_message_ = {
                "EditToolController",
                QT_TRANSLATE_NOOP("EditToolController", "Exporting the captured adjustments…")
            };
            break;
        default:
            break;
        }
    }
    owner_.tool_operation_running_ = running;
    emit owner_.statusTextChanged();
    emit owner_.busyChanged();
}
void EditToolController::reserveCommit(bool reserved) {
    owner_.tool_commit_reserved_ = reserved;
    emit owner_.stateBusyChanged();
    emit owner_.busyChanged();
    emit owner_.historyChanged();
}
void EditToolController::apply(const EditToolProtocol::Request& request) {
    if (proposal_id_.isEmpty() || request.proposal_id != proposal_id_)
        throw std::invalid_argument("preview this exact snapshot before applying its proposal");
    task_ = request;
    // Reserve before emitting activity/status signals, which can synchronously
    // re-enter Qt callbacks. Every observable apply state already owns the draft.
    reserveCommit(true);
    operationRunning(true);
    watcher_.setFuture(
        QtConcurrent::run([backend = backend_, capture = *snapshot_, stack = candidate_] {
            EditToolTaskResult result;
            try {
                const auto& id = capture.identity;
                result.commit = backend->commitEditToolDraft(
                    id.value("photoId").toString(),
                    capture.source,
                    id.value("representationId").toString(),
                    id.value("baseCommitId").toString(),
                    id.value("workingCommitId").toString(),
                    id.value("activeVariantId").toString(),
                    stack
                );
                // These presentation facts are deliberately absent from Recipe bytes.
                // These Grade adjustments preserve Foundation, including source As Shot metadata.
                result.commit.grade_stack.foundation = capture.stack.foundation;
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
                result.code = "commit_rejected";
            }
            return result;
        })
    );
}
void EditToolController::exportFile(const EditToolProtocol::Request& request) {
    const auto& id = snapshot_->identity;
    const auto commit = id.value("workingCommitId").toString();
    if (commit.isEmpty() || commit != id.value("baseCommitId").toString())
        throw std::invalid_argument(
            "export requires an applied committed Recipe, not a neutral or historical draft"
        );
    const auto output = newOutput(request.output_path, false);
    const BackendDurableExportTarget target{
        id.value("photoId").toString(),
        snapshot_->source,
        output,
        commit,
        id.value("representationId").toString()
    };
    task_ = request;
    operationRunning(true);
    // Subsequent human editing is allowed: the queue receives this immutable
    // commit identity, never closes the owner or chooses a later working head.
    watcher_.setFuture(QtConcurrent::run([backend = backend_, target] {
        EditToolTaskResult result;
        try {
            auto export_backend =
                std::shared_ptr<ExportBackend>(backend, &backend->exportBackend());
            const auto options = ExportSettingsCodec::fromVariantMap({{"format", "png"}});
            const auto exported = ExportTaskRunner::runDurableExport(
                export_backend,
                {target},
                ExportSettingsCodec::toDurableJson(options),
                false,
                std::make_shared<std::atomic_bool>(false),
                [](int, int, int, int, const QString&) {}
            );
            if (exported.completed != 1
                || exported.destination_paths != QStringList{target.output_path})
                throw std::runtime_error(
                    exported.errors.isEmpty() ? "export did not complete"
                                              : exported.errors.join('\n').toStdString()
                );
            // Publication is already acknowledged. Optional readback metadata
            // cannot turn a successful write into a misleading export failure.
            result.artifact = {
                {"path", target.output_path},
                {"format", "png"},
                {"commitId", target.recipe_commit_id},
                {"published", true}
            };
            QFile file(target.output_path);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            const QSize size = QImageReader(target.output_path).size();
            if (file.open(QIODevice::ReadOnly) && hash.addData(&file) && size.isValid()) {
                result.artifact =
                    artifact(target.output_path, "png", size, hash.result(), file.size());
                result.artifact.insert("commitId", target.recipe_commit_id);
                result.artifact.insert("published", true);
                result.artifact.insert("metadataSource", "post_publication_readback");
            } else {
                result.artifact.insert("metadataUnavailable", true);
            }
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
            result.code = "export_failed";
        }
        return result;
    }));
}
void EditToolController::finishTask() {
    auto result = watcher_.result();
    const auto request = *task_;
    const bool still_current = current();
    task_.reset();
    preview_stage_.reset();
    preview_token_ = 0;
    if (request.command == EditToolProtocol::Command::Apply) {
        if (result.error.isEmpty()) {
            const auto& receipt = result.commit;
            // No database readback here. Report the acknowledged transaction
            // even if another session has already advanced the durable head.
            owner_.applyCommittedGradeStack(
                std::move(result.commit.grade_stack),
                receipt.commit_id,
                snapshot_->stack,
                candidate_node_,
                QStringLiteral("tool/adjustments")
            );
            for (auto& value : owner_.photo_variants_) {
                auto variant = value.toMap();
                if (variant.value("variantId").toString() == receipt.variant_id) {
                    variant.insert("headCommitId", receipt.commit_id);
                    variant.insert("hasHead", true);
                    value = variant;
                }
            }
            emit owner_.photoVariantsChanged();
            reserveCommit(false);
            proposal_id_.clear();
            snapshot_.reset();
            candidate_ = {};
            candidate_changes_ = {};
            operationRunning(false);
            emit reply(
                EditToolProtocol::success(
                    request.id,
                    {{"photoId", receipt.photo_id},
                     {"representationId", receipt.representation_id},
                     {"activeVariantId", receipt.variant_id},
                     {"commitId", receipt.commit_id},
                     {"recipeId", receipt.recipe_id},
                     {"snapshotDigest", receipt.snapshot_digest},
                     {"undoSteps", 1}}
                )
            );
            return;
        }
        reserveCommit(false);
        proposal_id_.clear();
    }
    operationRunning(false);
    if (!result.error.isEmpty()) {
        fail(request.id, result.code, result.error);
        return;
    }
    if (request.command == EditToolProtocol::Command::Preview) {
        proposal_id_ = still_current ? token() : QString{};
        emit reply(
            EditToolProtocol::success(
                request.id,
                {{"artifact", result.artifact},
                 {"proposalId", proposal_id_},
                 {"current", still_current},
                 {"changes", candidate_changes_},
                 {"expected", request.expected}}
            )
        );
    } else {
        emit reply(EditToolProtocol::success(request.id, {{"artifact", result.artifact}}));
    }
}
