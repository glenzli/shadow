#include "backend/export_settings_codec.hpp"
#include "edit_controller.hpp"
#include "pipeline_run_controller.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <algorithm>
#include <stdexcept>

void PipelineRunController::saveDocument(const QUrl& url) {
    if ((!interactive() && !request_.document_protocol) || !url.isLocalFile() || busy() || !started_
        || inputs_.isEmpty() || terminal_ || editor_.autosaveFailed())
        return;
    const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
    if (!path.endsWith(".shdw", Qt::CaseInsensitive)) {
        error_text_ = tr("Choose a Shadow document (.shdw) file.");
        emit changed();
        return;
    }
    finished_ = false;
    completed_paths_.clear();
    saving_path_ = path;
    document_save_pending_ = true;
    completion_requested_ = true;
    error_text_.clear();
    editor_.finishDocumentGesture();
    editor_.closePhoto();
    if (!editor_.active() && !editor_.busy())
        QMetaObject::invokeMethod(this, &PipelineRunController::beginExport, Qt::QueuedConnection);
    emit changed();
}

void PipelineRunController::beginDocumentSave() {
    if (document_watcher_.isRunning())
        return;
    cancellation_token_ = std::make_shared<std::atomic_bool>(false);
    document_watcher_.setFuture(
        QtConcurrent::run([backend = backend_,
                           inputs = inputs_,
                           root = runtime_root_,
                           path = saving_path_,
                           expected = saving_path_ == document_path_ ? document_digest_ : QString{},
                           cancel = cancellation_token_] {
            DocumentSaveResult result;
            try {
                backend->flushDocumentHistory();
                QTemporaryDir render_directory(QDir(root).filePath("render-stage-XXXXXX"));
                if (!render_directory.isValid())
                    throw std::runtime_error("Cannot create document render staging");
                QVector<BackendDurableExportTarget> targets;
                for (const auto& input : inputs)
                    targets.push_back(
                        {input.photo_id,
                         input.source_path,
                         QDir(render_directory.path()).filePath(input.photo_id + ".png")}
                    );
                const auto export_backend =
                    std::shared_ptr<ExportBackend>(backend, &backend->exportBackend());
                const auto options = ExportSettingsCodec::fromVariantMap(
                    {{"format", "png"}, {"colorSpace", "srgb"}, {"bitDepth", 8}}
                );
                const auto rendered = ExportTaskRunner::runDurableExport(
                    export_backend,
                    targets,
                    ExportSettingsCodec::toDurableJson(options),
                    false,
                    cancel,
                    [](int, int, int, int, const QString&) {}
                );
                if (rendered.failed || rendered.cancelled || rendered.completed != targets.size())
                    throw std::runtime_error(
                        rendered.errors.isEmpty() ? "Document rendering did not complete"
                                                  : rendered.errors.join('\n').toStdString()
                    );
                if (cancel->load())
                    throw std::runtime_error("Document saving cancelled");
                for (const auto& input : inputs) {
                    const auto state = backend->photoEditState(input.photo_id, input.source_path);
                    const EditHistorySnapshot snapshot{state.grade_stack, state.base_commit_id};
                    backend->recordDocumentHistory(
                        input.photo_id,
                        input.source_path,
                        4,
                        {snapshot, snapshot, "selection"}
                    );
                    backend->flushDocumentHistory();
                }
                QDir old(QDir(root).filePath("renders"));
                if (old.exists() && !old.removeRecursively())
                    throw std::runtime_error("Cannot replace private document renders");
                if (!QDir().rename(render_directory.path(), old.path()))
                    throw std::runtime_error("Cannot stage document renders");
                result.digest = DesktopBackend::packDocument(root, path, expected);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}

void PipelineRunController::finishDocumentSave() {
    const auto result = document_watcher_.result();
    document_save_pending_ = false;
    completion_requested_ = false;
    cancellation_token_.reset();
    if (!result.digest.isEmpty()) {
        document_dirty_ = false;
        document_path_ = saving_path_;
        document_digest_ = result.digest;
        request_.document_digest = result.digest;
        if (request_.document_protocol) {
            finish("completed", 0);
            return;
        }
        if (cancel_requested_) {
            finish("completed", 0);
            return;
        }
        selectPhoto(std::max(0, current_index_));
        emit documentSaved();
    } else {
        error_text_ = tr("Could not save the editable document · %1").arg(result.error);
        if (cancel_requested_ || !interactive()) {
            finish(
                cancel_requested_ ? "cancelled" : "failed",
                cancel_requested_ ? 2 : 1,
                {error_text_}
            );
            return;
        }
        selectPhoto(std::max(0, current_index_));
        error_text_ = tr("Could not save the editable document · %1").arg(result.error);
    }
    emit changed();
}
