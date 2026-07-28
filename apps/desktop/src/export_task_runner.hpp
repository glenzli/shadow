#pragma once

#include "backend/export_backend.hpp"

#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

struct ExportTaskResult final {
    int requested = 0;
    int completed = 0;
    int failed = 0;
    bool cancelled = false;
    bool recovered_on_startup = false;
    int paused_conflicts = 0;
    int cancelled_items = 0;
    QString job_id;
    QStringList destination_paths;
    QStringList errors;
};

using ExportProgressReporter = std::function<void(
    int completed,
    int failed,
    int current,
    int total,
    const QString& current_title
)>;

namespace ExportTaskRunner {

[[nodiscard]] ExportTaskResult runDurableExport(
    const std::shared_ptr<ExportBackend>& backend,
    const QVector<BackendDurableExportTarget>& targets,
    const QString& settings_json,
    bool recover_existing,
    const std::shared_ptr<std::atomic_bool>& cancellation_token,
    const ExportProgressReporter& report_progress
);

} // namespace ExportTaskRunner
