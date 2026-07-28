#include "export_controller.hpp"

#include "backend/export_backend.hpp"
#include "backend/export_settings_codec.hpp"
#include "desktop_backend.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>

namespace {

constexpr auto export_presets_settings_key = "export/presets_json";

[[nodiscard]] int checked_export_count(const qsizetype count) {
    if (count < 0 || count > std::numeric_limits<int>::max()) {
        throw std::length_error("export selection exceeds the desktop count limit");
    }
    return static_cast<int>(count);
}

struct ExportPhoto final {
    QString photo_id;
    QString source_path;
    QString title;
};

using ExportProgressReporter = std::function<void(
    int completed,
    int failed,
    int current,
    int total,
    const QString& current_title
)>;

[[nodiscard]] QString safe_stem(const QString& requested, const QString& source_path) {
    QString stem = requested.trimmed();
    if (stem.isEmpty()) {
        stem = QFileInfo(source_path).completeBaseName();
    }
    stem.replace(
        QRegularExpression(QStringLiteral(R"([/\\:\*\?"<>\|])")),
        QStringLiteral("_")
    );
    stem = stem.trimmed();
    return stem.isEmpty() ? QStringLiteral("Shadow export") : stem;
}

[[nodiscard]] QString unique_destination(
    const QDir& folder,
    const QString& stem,
    const QString& suffix,
    const QString& extension,
    QSet<QString>& reserved_destinations
) {
    const QString base = stem + suffix;
    QString candidate = folder.filePath(base + QStringLiteral(".") + extension);
    for (int copy = 2;
         QFileInfo::exists(candidate) || reserved_destinations.contains(candidate);
         ++copy) {
        candidate = folder.filePath(
            QStringLiteral("%1-%2.%3").arg(base).arg(copy).arg(extension)
        );
    }
    reserved_destinations.insert(candidate);
    return candidate;
}

[[nodiscard]] std::shared_ptr<ExportBackend> shared_export_backend(
    const std::shared_ptr<DesktopBackend>& backend
) {
    if (!backend) {
        throw std::invalid_argument("ExportController requires a desktop backend");
    }
    return std::shared_ptr<ExportBackend>(backend, &backend->exportBackend());
}

[[nodiscard]] int checked_export_count(const std::uint32_t count) {
    if (count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("durable export count exceeds the desktop count limit");
    }
    return static_cast<int>(count);
}

[[nodiscard]] QString source_title(const BackendDurableExportItem& item) {
    const QString title = QFileInfo(item.source_path).fileName();
    return title.isEmpty() ? item.photo_id : title;
}

void apply_durable_progress(
    ExportTaskResult& result,
    const BackendDurableExportProgress& progress
) {
    result.completed = checked_export_count(progress.completed);
    result.failed = checked_export_count(
        progress.failed + progress.paused_conflict
    );
    result.paused_conflicts = checked_export_count(progress.paused_conflict);
    result.cancelled_items = checked_export_count(progress.cancelled);
    result.requested = checked_export_count(progress.total);
}

[[nodiscard]] int durable_current_count(
    const ExportTaskResult& result,
    const BackendDurableExportProgress& progress
) {
    const int active = progress.active > 0 ? 1 : 0;
    return std::min(
        result.requested,
        result.completed + result.failed + result.cancelled_items + active
    );
}

[[nodiscard]] ExportTaskResult run_durable_export(
    const std::shared_ptr<ExportBackend>& backend,
    const QVector<BackendDurableExportTarget>& targets,
    const QString& settings_json,
    const bool recover_existing,
    const std::shared_ptr<std::atomic_bool>& cancellation_token,
    const ExportProgressReporter& report_progress
) {
    ExportTaskResult result;
    result.recovered_on_startup = recover_existing;
    QString selected_job_id;
    try {
        if (recover_existing) {
            const BackendDurableExportRecovery recovery =
                backend->recoverDurableExportQueue();
            result.requested = checked_export_count(recovery.queued_items);
            if (result.requested == 0) {
                return result;
            }
        } else {
            result.requested = checked_export_count(targets.size());
            if (cancellation_token->load(std::memory_order_relaxed)) {
                result.cancelled = true;
                return result;
            }
            const BackendDurableExportJob job =
                backend->enqueueDurableExportJob(targets, settings_json);
            selected_job_id = job.job_id;
            result.job_id = job.job_id;
            result.requested = checked_export_count(job.item_count);
        }

        for (;;) {
            if (cancellation_token->load(std::memory_order_relaxed)) {
                if (!selected_job_id.isEmpty()) {
                    backend->cancelDurableExportJob(selected_job_id);
                    const auto progress = backend->durableExportProgress(selected_job_id);
                    apply_durable_progress(result, progress);
                }
                result.cancelled = true;
                break;
            }

            if (!selected_job_id.isEmpty()) {
                const auto progress = backend->durableExportProgress(selected_job_id);
                apply_durable_progress(result, progress);
                if (progress.queued == 0 && progress.active == 0) {
                    break;
                }
            }

            const auto item = backend->claimNextDurableExportItem();
            if (!item.has_value()) {
                break;
            }
            const QString title = source_title(*item);
            if (selected_job_id.isEmpty()) {
                report_progress(
                    result.completed,
                    result.failed,
                    std::min(result.requested, result.completed + result.failed + 1),
                    result.requested,
                    title
                );
            } else {
                const auto progress = backend->durableExportProgress(selected_job_id);
                apply_durable_progress(result, progress);
                report_progress(
                    result.completed,
                    result.failed,
                    std::max(1, durable_current_count(result, progress)),
                    result.requested,
                    title
                );
            }

            bool item_succeeded = false;
            try {
                const auto receipt = backend->executeDurableExportItem(*item);
                item_succeeded = true;
                if (selected_job_id.isEmpty() || item->job_id == selected_job_id) {
                    result.destination_paths.push_back(receipt.destination_path);
                }
            } catch (const std::exception& error) {
                result.errors.push_back(
                    QStringLiteral("%1 · %2")
                        .arg(title, QString::fromUtf8(error.what()))
                );
            }

            if (selected_job_id.isEmpty()) {
                // Startup recovery deliberately has no selected job. Its
                // count is a practical task-center estimate; durable per-job
                // state remains the source of truth.
                if (item_succeeded) {
                    ++result.completed;
                } else {
                    ++result.failed;
                }
                report_progress(
                    result.completed,
                    result.failed,
                    std::min(result.requested, result.completed + result.failed),
                    result.requested,
                    title
                );
            } else {
                const auto progress = backend->durableExportProgress(selected_job_id);
                apply_durable_progress(result, progress);
                report_progress(
                    result.completed,
                    result.failed,
                    std::max(1, durable_current_count(result, progress)),
                    result.requested,
                    title
                );
            }
        }

        if (!selected_job_id.isEmpty()) {
            const auto progress = backend->durableExportProgress(selected_job_id);
            apply_durable_progress(result, progress);
            result.cancelled = result.cancelled || progress.cancelled > 0;
        }
    } catch (const std::exception& error) {
        result.errors.push_back(QString::fromUtf8(error.what()));
        if (result.requested == 0) {
            result.requested = 1;
        }
        result.failed = std::max(1, result.failed);
    }
    return result;
}

} // namespace

ExportController::ExportController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      export_backend_(shared_export_backend(backend)),
      settings_(isolated_settings_file.isEmpty()
              ? std::make_unique<QSettings>()
              : std::make_unique<QSettings>(
                    isolated_settings_file,
                    QSettings::IniFormat
                )) {
    const QByteArray stored =
        settings_->value(QString::fromLatin1(export_presets_settings_key)).toByteArray();
    const auto parsed = QJsonDocument::fromJson(stored);
    presets_ = parsed.isArray() ? parsed.toVariant().toList() : defaultPresets();
    connect(
        &watcher_,
        &QFutureWatcher<ExportTaskResult>::finished,
        this,
        &ExportController::finishExport
    );
    // Recovery only touches the catalog from the background worker. It does
    // not make the first Library frame wait on SQLite or a source decode.
    QTimer::singleShot(0, this, &ExportController::startRecoveryDrain);
}

ExportController::~ExportController() {
    if (cancellation_token_) {
        cancellation_token_->store(true, std::memory_order_relaxed);
    }
    watcher_.waitForFinished();
}

bool ExportController::busy() const noexcept {
    return watcher_.isRunning();
}

QString ExportController::statusText() const {
    return status_text_;
}

int ExportController::completedCount() const noexcept {
    return completed_count_;
}

int ExportController::failedCount() const noexcept {
    return failed_count_;
}

int ExportController::currentCount() const noexcept {
    return current_count_;
}

int ExportController::totalCount() const noexcept {
    return total_count_;
}

bool ExportController::cancellationRequested() const noexcept {
    return cancellation_requested_;
}

QStringList ExportController::errors() const {
    return errors_;
}

QVariantList ExportController::presets() const {
    return presets_;
}

void ExportController::startRecoveryDrain() {
    if (busy()) {
        return;
    }
    completed_count_ = 0;
    failed_count_ = 0;
    current_count_ = 0;
    total_count_ = 0;
    cancellation_requested_ = false;
    errors_.clear();
    cancellation_token_ = std::make_shared<std::atomic_bool>(false);
    status_text_ = tr("Checking unfinished exports…");
    emit progressChanged();
    emit statusTextChanged();
    emit cancellationRequestedChanged();
    emit errorsChanged();
    emit busyChanged();
    const auto generation = ++export_generation_;
    const auto report_progress = [this, generation](
                                     const int completed,
                                     const int failed,
                                     const int current,
                                     const int total,
                                     const QString& current_title
                                 ) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, completed, failed, current, total, current_title] {
                applyProgress(
                    generation,
                    completed,
                    failed,
                    current,
                    total,
                    current_title
                );
            },
            Qt::QueuedConnection
        );
    };
    watcher_.setFuture(QtConcurrent::run(
        run_durable_export,
        export_backend_,
        QVector<BackendDurableExportTarget>{},
        QString{},
        true,
        cancellation_token_,
        report_progress
    ));
}

void ExportController::startExport(
    const QVariantList& targets,
    const QUrl& destination_folder,
    const QVariantMap& options
) {
    if (busy()) {
        return;
    }
    const QString folder_path = destination_folder.toLocalFile();
    if (folder_path.isEmpty() || !QDir(folder_path).exists()) {
        status_text_ = tr("Choose an export folder");
        emit statusTextChanged();
        return;
    }
    QVector<ExportPhoto> photos;
    photos.reserve(targets.size());
    QSet<QString> seen;
    for (const auto& value : targets) {
        const QVariantMap target = value.toMap();
        const QString photo_id = target.value(QStringLiteral("photoId")).toString();
        const QString source_path =
            target.value(QStringLiteral("sourcePath")).toString();
        if (photo_id.isEmpty() || source_path.isEmpty() || seen.contains(photo_id)) {
            continue;
        }
        seen.insert(photo_id);
        photos.push_back({
            .photo_id = photo_id,
            .source_path = source_path,
            .title = target.value(QStringLiteral("title")).toString(),
        });
    }
    if (photos.isEmpty()) {
        status_text_ = tr("Select at least one photo to export");
        emit statusTextChanged();
        return;
    }
    BackendExportOptions export_options;
    try {
        export_options = ExportSettingsCodec::fromVariantMap(options);
    } catch (const std::exception& error) {
        status_text_ = QString::fromUtf8(error.what());
        emit statusTextChanged();
        return;
    }
    const QString extension = export_options.format == QStringLiteral("png")
        ? QStringLiteral("png")
        : QStringLiteral("jpg");
    const QString suffix = export_options.filename_suffix;
    const QDir folder(QDir(folder_path).absolutePath());
    QSet<QString> reserved_destinations;
    QVector<BackendDurableExportTarget> durable_targets;
    durable_targets.reserve(photos.size());
    for (const auto& photo : photos) {
        durable_targets.push_back({
            .photo_id = photo.photo_id,
            .source_path = photo.source_path,
            .output_path = unique_destination(
                folder,
                safe_stem(photo.title, photo.source_path),
                suffix,
                extension,
                reserved_destinations
            ),
        });
    }
    const QString settings_json =
        ExportSettingsCodec::toDurableJson(export_options);
    completed_count_ = 0;
    failed_count_ = 0;
    current_count_ = 0;
    total_count_ = checked_export_count(photos.size());
    cancellation_requested_ = false;
    errors_.clear();
    cancellation_token_ = std::make_shared<std::atomic_bool>(false);
    status_text_ = tr("Queueing %1 photos…").arg(total_count_);
    emit progressChanged();
    emit statusTextChanged();
    emit cancellationRequestedChanged();
    emit errorsChanged();
    emit busyChanged();
    const auto generation = ++export_generation_;
    // The controller waits for its future in the destructor, so queued updates
    // cannot outlive this receiver. A generation still protects a new export
    // from late updates posted by a previous run.
    const auto report_progress = [this, generation](
                                     const int completed,
                                     const int failed,
                                     const int current,
                                     const int total,
                                     const QString& current_title
                                 ) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, completed, failed, current, total, current_title] {
                applyProgress(
                    generation,
                    completed,
                    failed,
                    current,
                    total,
                    current_title
                );
            },
            Qt::QueuedConnection
        );
    };
    watcher_.setFuture(QtConcurrent::run(
        run_durable_export,
        export_backend_,
        durable_targets,
        settings_json,
        false,
        cancellation_token_,
        report_progress
    ));
}

void ExportController::cancelExport() {
    if (!busy() || cancellation_requested_ || !cancellation_token_) {
        return;
    }
    cancellation_requested_ = true;
    cancellation_token_->store(true, std::memory_order_relaxed);
    status_text_ = tr("Cancelling after the current photo…");
    emit cancellationRequestedChanged();
    emit statusTextChanged();
}

QString ExportController::savePreset(
    const QString& name,
    const QVariantMap& options
) {
    const QString normalized_name = name.trimmed();
    if (normalized_name.isEmpty()) {
        return {};
    }
    QString id;
    for (const auto& value : presets_) {
        const QVariantMap preset = value.toMap();
        if (preset.value(QStringLiteral("name")).toString().compare(
                normalized_name,
                Qt::CaseInsensitive
            ) == 0) {
            id = preset.value(QStringLiteral("id")).toString();
            break;
        }
    }
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const QVariantMap normalized = ExportSettingsCodec::normalizedPreset(
        id,
        normalized_name,
        options
    );
    bool replaced = false;
    for (qsizetype index = 0; index < presets_.size(); ++index) {
        if (presets_[index].toMap().value(QStringLiteral("id")).toString() == id) {
            presets_[index] = normalized;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        presets_.push_back(normalized);
    }
    persistPresets();
    emit presetsChanged();
    return id;
}

void ExportController::removePreset(const QString& preset_id) {
    const auto before = presets_.size();
    presets_.erase(
        std::remove_if(
            presets_.begin(),
            presets_.end(),
            [&preset_id](const QVariant& value) {
                return value.toMap().value(QStringLiteral("id")).toString()
                    == preset_id;
            }
        ),
        presets_.end()
    );
    if (presets_.size() != before) {
        persistPresets();
        emit presetsChanged();
    }
}

void ExportController::finishExport() {
    const ExportTaskResult result = watcher_.result();
    completed_count_ = result.completed;
    failed_count_ = result.failed;
    current_count_ = std::min(
        result.requested,
        result.completed + result.failed + result.cancelled_items
    );
    total_count_ = result.requested;
    errors_ = result.errors;
    const bool was_cancelling = cancellation_requested_;
    cancellation_requested_ = false;
    cancellation_token_.reset();
    if (result.recovered_on_startup && result.requested == 0) {
        status_text_ = tr("No unfinished exports");
    } else if (result.recovered_on_startup && result.cancelled) {
        status_text_ = tr("Resuming exports paused");
    } else if (result.recovered_on_startup && result.paused_conflicts > 0) {
        status_text_ = tr("Resumed %1 photos · %2 need attention")
                           .arg(result.completed)
                           .arg(result.paused_conflicts);
    } else if (result.recovered_on_startup && result.failed == 0) {
        status_text_ = tr("Resumed %1 exports").arg(result.completed);
    } else if (result.recovered_on_startup) {
        status_text_ = tr("Resumed %1 exports · %2 failed")
                           .arg(result.completed)
                           .arg(result.failed);
    } else if (result.cancelled) {
        status_text_ = tr("Export cancelled · %1 exported · %2 failed")
                           .arg(result.completed)
                           .arg(result.failed);
    } else if (result.paused_conflicts > 0) {
        status_text_ = tr("Exported %1 photos · %2 need attention")
                           .arg(result.completed)
                           .arg(result.paused_conflicts);
    } else if (result.failed == 0) {
        status_text_ = tr("Exported %1 photos").arg(result.completed);
    } else {
        status_text_ = tr("Exported %1 photos · %2 failed")
                           .arg(result.completed)
                           .arg(result.failed);
    }
    emit progressChanged();
    emit statusTextChanged();
    if (was_cancelling) {
        emit cancellationRequestedChanged();
    }
    emit errorsChanged();
    emit busyChanged();
    if (!result.recovered_on_startup) {
        emit exportFinished(
            result.completed,
            result.failed,
            result.cancelled,
            result.destination_paths,
            result.errors
        );
    }
}

void ExportController::applyProgress(
    const quint64 generation,
    const int completed,
    const int failed,
    const int current,
    const int total,
    const QString& current_title
) {
    if (!busy() || generation != export_generation_) {
        return;
    }
    completed_count_ = completed;
    failed_count_ = failed;
    current_count_ = current;
    total_count_ = total;
    if (!cancellation_requested_) {
        status_text_ = tr("Exporting %1 of %2 · %3")
                           .arg(current)
                           .arg(total)
                           .arg(current_title);
        emit statusTextChanged();
    }
    emit progressChanged();
}

void ExportController::persistPresets() {
    settings_->setValue(
        QString::fromLatin1(export_presets_settings_key),
        QJsonDocument::fromVariant(presets_).toJson(QJsonDocument::Compact)
    );
    settings_->sync();
}

QVariantList ExportController::defaultPresets() {
    return {
        ExportSettingsCodec::normalizedPreset(
            QStringLiteral("builtin-full-jpeg"),
            tr("Full-size JPEG"),
            {
                {QStringLiteral("format"), QStringLiteral("jpeg")},
                {QStringLiteral("maxEdge"), 0},
                {QStringLiteral("quality"), 92},
            }
        ),
        ExportSettingsCodec::normalizedPreset(
            QStringLiteral("builtin-web-jpeg"),
            tr("Web JPEG"),
            {
                {QStringLiteral("format"), QStringLiteral("jpeg")},
                {QStringLiteral("maxEdge"), 2560},
                {QStringLiteral("quality"), 86},
            }
        ),
        ExportSettingsCodec::normalizedPreset(
            QStringLiteral("builtin-png"),
            tr("Full-size PNG"),
            {
                {QStringLiteral("format"), QStringLiteral("png")},
                {QStringLiteral("maxEdge"), 0},
                {QStringLiteral("quality"), 100},
            }
        ),
    };
}
