#include "pipeline_run_controller.hpp"
#include "backend/export_settings_codec.hpp"
#include "edit_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>
#include <cstdlib>
#include <exception>

PipelineRunController::PipelineRunController(
    std::shared_ptr<DesktopBackend> backend,
    EditController& editor,
    PipelineLaunchRequest request,
    QObject* parent
) :
    QObject(parent), backend_(std::move(backend)), editor_(editor), inspection_(backend_),
    request_(std::move(request)) {
    connect(
        &inspection_,
        &ReviewPhotoInspectionCoordinator::stateChanged,
        this,
        &PipelineRunController::changed
    );
    connect(
        &admission_watcher_,
        &QFutureWatcher<AdmissionResult>::finished,
        this,
        &PipelineRunController::finishAdmission
    );
    connect(
        &export_watcher_,
        &QFutureWatcher<ExportTaskResult>::finished,
        this,
        &PipelineRunController::finishExport
    );
    const auto update = [this] {
        emit changed();
        if (completion_requested_ && !editor_.active() && !editor_.busy())
            QTimer::singleShot(0, this, &PipelineRunController::beginExport);
    };
    connect(&editor_, &EditController::activeChanged, this, update);
    connect(&editor_, &EditController::busyChanged, this, update);
    connect(&editor_, &EditController::stateBusyChanged, this, update);
    connect(&editor_, &EditController::statusTextChanged, this, &PipelineRunController::changed);
    connect(&editor_, &EditController::sourceIdentityChanged, this, [this] {
        if (editor_.active())
            inspection_.request(editor_.photoId(), editor_.representationId());
        else
            inspection_.clear();
        for (qsizetype i = 0; i < inputs_.size(); ++i) {
            if (inputs_[i].photo_id == editor_.photoId()) {
                current_index_ = static_cast<int>(i);
                break;
            }
        }
        emit changed();
    });
    connect(&editor_, &EditController::autosaveFailedChanged, this, [this] {
        if (editor_.autosaveFailed())
            persistenceFailed();
    });
    connect(
        &editor_,
        &EditController::closeSaveFailed,
        this,
        &PipelineRunController::persistenceFailed
    );
    connect(
        &editor_,
        &EditController::photoSwitchSaveFailed,
        this,
        &PipelineRunController::persistenceFailed
    );
}

PipelineRunController::~PipelineRunController() {
    if (admission_cancellation_)
        admission_cancellation_->store(true, std::memory_order_relaxed);
    if (cancellation_token_)
        cancellation_token_->store(true, std::memory_order_relaxed);
    admission_watcher_.waitForFinished();
    export_watcher_.waitForFinished();
}

bool PipelineRunController::busy() const noexcept {
    return admission_watcher_.isRunning() || exporting() || completion_requested_
           || editor_.stateBusy();
}

bool PipelineRunController::navigationEnabled() const noexcept {
    return !terminal_ && !finished_ && !completion_requested_ && !exporting()
           && !admission_watcher_.isRunning() && (editor_.active() || !editor_.stateBusy());
}

bool PipelineRunController::currentPhotoEditable() const noexcept {
    return navigationEnabled() && current_index_ >= 0 && current_index_ < request_.photos.size()
           && !completed_paths_.contains(request_.photos[current_index_].output_path);
}

QVariantList PipelineRunController::photos() const {
    QVariantList result;
    for (const auto& input : inputs_)
        result.append(
            QVariantMap{
                {QStringLiteral("name"), QFileInfo(input.source_path).fileName()},
                {QStringLiteral("path"), input.source_path},
                {QStringLiteral("photoId"), input.photo_id},
            }
        );
    return result;
}

QString PipelineRunController::statusText() const {
    if (finished_)
        return tr("Exported %1 photos. You can close this session.").arg(completedCount());
    if (admission_watcher_.isRunning())
        return tr("Opening photos…");
    if (exporting())
        return tr("Exporting %1 of %2 · %3")
            .arg(progress_current_)
            .arg(progress_total_)
            .arg(progress_title_);
    if (completion_requested_)
        return tr("Saving adjustments before export…");
    if (inputs_.isEmpty())
        return tr("Open photos to start an independent editing session.");
    if (current_index_ >= 0 && current_index_ < request_.photos.size()
        && completed_paths_.contains(request_.photos[current_index_].output_path))
        return tr("Already exported. Select an unfinished photo to continue editing.");
    return editor_.statusText();
}

void PipelineRunController::start() {
    if (started_ || terminal_)
        return;
    started_ = true;
    QStringList paths;
    for (const auto& photo : request_.photos)
        paths.append(photo.input_path);
    if (!paths.isEmpty())
        admit(paths);
    else
        emit changed();
}

void PipelineRunController::addFiles(const QList<QUrl>& files) {
    if (!interactive() || busy() || terminal_ || finished_ || completedCount() > 0)
        return;
    QStringList paths;
    QSet<QString> seen;
    for (const auto& input : inputs_)
        seen.insert(input.source_path);
    for (const auto& file : files) {
        const QFileInfo info(file.toLocalFile());
        const QString path = info.canonicalFilePath();
        if (!file.isLocalFile() || !info.isFile()) {
            error_text_ = tr("Choose existing local photo files.");
            emit changed();
            return;
        }
        if (!seen.contains(path)) {
            seen.insert(path);
            paths.append(path);
        }
    }
    if (seen.size() > 256) {
        error_text_ = tr("A session can contain up to 256 photos.");
        emit changed();
        return;
    }
    if (!paths.isEmpty())
        admit(paths);
}

void PipelineRunController::admit(const QStringList& paths) {
    error_text_.clear();
    admission_cancellation_ = std::make_shared<std::atomic_bool>(false);
    admission_watcher_.setFuture(
        QtConcurrent::run([backend = backend_, paths, cancel = admission_cancellation_] {
            AdmissionResult result;
            for (const auto& path : paths) {
                if (cancel->load(std::memory_order_relaxed))
                    break;
                try {
                    result.inputs.append(backend->admitPipelineInput(path));
                } catch (const std::exception& error) {
                    result.errors.append(
                        QFileInfo(path).fileName() + QStringLiteral(" · ")
                        + QString::fromUtf8(error.what())
                    );
                }
            }
            return result;
        })
    );
    emit changed();
}

void PipelineRunController::finishAdmission() {
    const auto result = admission_watcher_.result();
    admission_cancellation_.reset();
    if (cancel_requested_) {
        finish(QStringLiteral("cancelled"), 2);
        return;
    }
    if (!result.errors.isEmpty() && !interactive()) {
        finish(QStringLiteral("failed"), EXIT_FAILURE, result.errors);
        return;
    }
    inputs_ += result.inputs;
    if (interactive()) {
        request_.photos.clear();
        for (const auto& input : inputs_)
            request_.photos.push_back({input.source_path, {}});
    }
    if (!editor_.active() && !inputs_.isEmpty())
        selectPhoto(0);
    if (!result.errors.isEmpty())
        error_text_ = result.errors.join(QLatin1Char('\n'));
    emit changed();
}

void PipelineRunController::selectPhoto(const int index) {
    if (index < 0 || index >= inputs_.size() || !navigationEnabled())
        return;
    const auto& input = inputs_[index];
    if (!editor_.openPhoto(input.photo_id, input.representation_id, input.source_path, input.title))
        error_text_ = editor_.statusText();
    else if (!editor_.autosaveFailed())
        error_text_.clear();
    emit changed();
}

bool PipelineRunController::configureExport(const QUrl& folder, const QVariantMap& options) {
    if (!interactive() || busy() || finished_ || completedCount() > 0)
        return false;
    const QDir directory(folder.toLocalFile());
    if (!folder.isLocalFile() || !directory.exists()) {
        error_text_ = tr("Choose an existing output folder.");
        emit changed();
        return false;
    }
    try {
        const auto settings = ExportSettingsCodec::fromVariantMap(options);
        if (settings.format == QStringLiteral("dng"))
            return false;
        const QString extension = settings.format == QStringLiteral("jpeg") ? QStringLiteral("jpg")
                                  : settings.format == QStringLiteral("tiff")
                                      ? QStringLiteral("tif")
                                      : settings.format;
        QSet<QString> reserved;
        for (auto& photo : request_.photos) {
            const QString stem =
                QFileInfo(photo.input_path).completeBaseName() + QStringLiteral("-edited");
            QString output;
            int suffix = 0;
            do {
                output = directory.filePath(
                    stem + (suffix == 0 ? QString{} : QStringLiteral("-%1").arg(suffix))
                    + QLatin1Char('.') + extension
                );
                ++suffix;
            } while (QFileInfo::exists(output) || QFileInfo(output).isSymLink()
                     || reserved.contains(output.toCaseFolded()));
            photo.output_path = output;
            reserved.insert(output.toCaseFolded());
        }
        output_folder_ = directory.absolutePath();
        request_.export_options = options;
        error_text_.clear();
        emit changed();
        return true;
    } catch (const std::exception& error) {
        error_text_ = QString::fromUtf8(error.what());
        emit changed();
        return false;
    }
}

void PipelineRunController::complete() {
    if (terminal_ || finished_ || !started_ || busy() || inputs_.isEmpty())
        return;
    for (const auto& photo : request_.photos)
        if (photo.output_path.isEmpty())
            return;
    if (editor_.autosaveFailed()) {
        persistenceFailed();
        return;
    }
    completion_requested_ = true;
    error_text_.clear();
    editor_.closePhoto();
    if (!editor_.active() && !editor_.busy())
        QTimer::singleShot(0, this, &PipelineRunController::beginExport);
    emit changed();
}

void PipelineRunController::persistenceFailed() {
    completion_requested_ = false;
    editor_.cancelPendingPhotoOpen();
    error_text_ = tr("Adjustments could not be saved. Retry saving before switching or exporting.");
    emit changed();
}

void PipelineRunController::beginExport() {
    if (terminal_ || !completion_requested_ || exporting() || editor_.active() || editor_.busy())
        return;
    try {
        const auto options = ExportSettingsCodec::fromVariantMap(request_.export_options);
        const auto backend = std::shared_ptr<ExportBackend>(backend_, &backend_->exportBackend());
        QVector<BackendDurableExportTarget> targets;
        for (qsizetype i = 0; i < inputs_.size(); ++i) {
            const auto& output = request_.photos[i].output_path;
            if (!completed_paths_.contains(output))
                targets.push_back({inputs_[i].photo_id, inputs_[i].source_path, output});
        }
        cancellation_token_ = std::make_shared<std::atomic_bool>(false);
        progress_current_ = 0;
        progress_total_ = static_cast<int>(targets.size());
        const QPointer<PipelineRunController> self(this);
        ExportProgressReporter progress =
            [self](int, int, int current, int total, const QString& title) {
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self,
                    [self, current, total, title] {
                        if (!self)
                            return;
                        self->progress_current_ = current;
                        self->progress_total_ = total;
                        self->progress_title_ = title;
                        emit self->changed();
                    },
                    Qt::QueuedConnection
                );
            };
        export_watcher_.setFuture(
            QtConcurrent::run(
                ExportTaskRunner::runDurableExport,
                backend,
                targets,
                ExportSettingsCodec::toDurableJson(options),
                false,
                cancellation_token_,
                progress
            )
        );
    } catch (const std::exception& error) {
        completion_requested_ = false;
        error_text_ = QString::fromUtf8(error.what());
        if (!interactive())
            finish(QStringLiteral("failed"), EXIT_FAILURE, {error_text_});
        else
            selectPhoto(std::max(0, current_index_));
    }
    emit changed();
}

void PipelineRunController::finishExport() {
    const auto result = export_watcher_.result();
    cancellation_token_.reset();
    completion_requested_ = false;
    for (const auto& path : result.destination_paths)
        completed_paths_.insert(path);
    if (cancel_requested_) {
        finish(QStringLiteral("cancelled"), 2, result.errors);
        return;
    }
    if (completedCount() == photoCount() && result.failed == 0 && !result.cancelled) {
        if (!interactive()) {
            finish(QStringLiteral("completed"), EXIT_SUCCESS);
            return;
        }
        finished_ = true;
    } else if (!interactive()) {
        finish(
            result.cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed"),
            result.cancelled ? 2 : EXIT_FAILURE,
            result.errors
        );
        return;
    } else {
        for (qsizetype i = 0; i < request_.photos.size(); ++i) {
            if (!completed_paths_.contains(request_.photos[i].output_path)) {
                selectPhoto(static_cast<int>(i));
                break;
            }
        }
        error_text_ = result.cancelled ? tr("Export stopped. Completed files are kept; you can "
                                            "retry the remaining photos.")
                                       : result.errors.join(QLatin1Char('\n'));
        if (error_text_.isEmpty())
            error_text_ = tr("Export did not complete. Check the output folder and retry.");
    }
    emit changed();
}

void PipelineRunController::stopExport() {
    if (cancellation_token_)
        cancellation_token_->store(true, std::memory_order_relaxed);
}

void PipelineRunController::cancel() {
    if (terminal_)
        return;
    cancel_requested_ = true;
    if (admission_cancellation_)
        admission_cancellation_->store(true, std::memory_order_relaxed);
    if (admission_watcher_.isRunning() || exporting()) {
        stopExport();
        emit changed();
        return;
    }
    finish(
        finished_ ? QStringLiteral("completed") : QStringLiteral("cancelled"),
        finished_ ? EXIT_SUCCESS : 2
    );
}

void PipelineRunController::finish(
    const QString& outcome,
    const int exit_code,
    const QStringList& errors
) {
    if (terminal_)
        return;
    const bool written = writePipelineResult(request_, outcome, errors, completed_paths_);
    terminal_ = true;
    emit changed();
    QCoreApplication::exit(written ? exit_code : EXIT_FAILURE);
}
