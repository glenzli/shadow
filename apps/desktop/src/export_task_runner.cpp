#include "export_task_runner.hpp"

#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

[[nodiscard]] int checked_export_count(const qsizetype count) {
    if (count < 0 || count > std::numeric_limits<int>::max()) {
        throw std::length_error("export selection exceeds the desktop count limit");
    }
    return static_cast<int>(count);
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

} // namespace

ExportTaskResult ExportTaskRunner::runDurableExport(
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
                    const auto progress =
                        backend->durableExportProgress(selected_job_id);
                    apply_durable_progress(result, progress);
                }
                result.cancelled = true;
                break;
            }

            if (!selected_job_id.isEmpty()) {
                const auto progress =
                    backend->durableExportProgress(selected_job_id);
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
                    std::min(
                        result.requested,
                        result.completed + result.failed + 1
                    ),
                    result.requested,
                    title
                );
            } else {
                const auto progress =
                    backend->durableExportProgress(selected_job_id);
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
                if (selected_job_id.isEmpty()
                    || item->job_id == selected_job_id) {
                    result.destination_paths.push_back(
                        receipt.destination_path
                    );
                }
            } catch (const std::exception& error) {
                result.errors.push_back(
                    QStringLiteral("%1 · %2")
                        .arg(title, QString::fromUtf8(error.what()))
                );
            }

            if (selected_job_id.isEmpty()) {
                // Startup recovery has no selected job. Its count is a
                // task-center estimate; durable per-job state remains the
                // source of truth.
                if (item_succeeded) {
                    ++result.completed;
                } else {
                    ++result.failed;
                }
                report_progress(
                    result.completed,
                    result.failed,
                    std::min(
                        result.requested,
                        result.completed + result.failed
                    ),
                    result.requested,
                    title
                );
            } else {
                const auto progress =
                    backend->durableExportProgress(selected_job_id);
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
            const auto progress =
                backend->durableExportProgress(selected_job_id);
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
