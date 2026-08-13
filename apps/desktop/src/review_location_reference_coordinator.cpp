#include "review_location_reference_coordinator.hpp"

#include <QVariantMap>
#include <QUrl>
#include <QtConcurrentRun>

#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] QVariantMap library_variant(const BackendLocationReferenceLibrary& library) {
    return {
        {QStringLiteral("id"), library.id},
        {QStringLiteral("rootPath"), library.root_path},
        {QStringLiteral("rootUrl"), QUrl::fromLocalFile(library.root_path).toString()},
        {QStringLiteral("clockOffsetSeconds"), library.clock_offset_seconds},
        {QStringLiteral("indexedAtUnixMs"), library.indexed_at_unix_ms},
        {QStringLiteral("anchorCount"), QVariant::fromValue<qulonglong>(library.anchor_count)},
    };
}

} // namespace

ReviewLocationReferenceCoordinator::ReviewLocationReferenceCoordinator(
    Operations operations,
    QObject* const parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.libraries || !operations_.add_or_rescan || !operations_.remove) {
        throw std::invalid_argument("complete location-reference operations are required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLocationReferenceCoordinator::finishTask
    );
}

ReviewLocationReferenceCoordinator::~ReviewLocationReferenceCoordinator() {
    watcher_.waitForFinished();
}

QVariantList ReviewLocationReferenceCoordinator::libraries() const {
    QVariantList result;
    result.reserve(libraries_.size());
    for (const auto& library : libraries_) {
        result.push_back(library_variant(library));
    }
    return result;
}

bool ReviewLocationReferenceCoordinator::busy() const noexcept {
    return task_running_;
}

QString ReviewLocationReferenceCoordinator::errorText() const {
    return error_text_;
}

void ReviewLocationReferenceCoordinator::refresh() {
    if (task_running_) {
        refresh_pending_ = true;
        return;
    }
    startTask(TaskAction::Refresh);
}

void ReviewLocationReferenceCoordinator::addOrRescan(
    const QString& root_path,
    const std::int64_t clock_offset_seconds
) {
    const QString normalized = root_path.trimmed();
    if (task_running_ || normalized.isEmpty()) {
        return;
    }
    startTask(TaskAction::AddOrRescan, normalized, clock_offset_seconds);
}

void ReviewLocationReferenceCoordinator::remove(const QString& id) {
    const QString normalized = id.trimmed();
    if (task_running_ || normalized.isEmpty()) {
        return;
    }
    startTask(TaskAction::Remove, {}, 0, normalized);
}

ReviewLocationReferenceCoordinator::TaskResult ReviewLocationReferenceCoordinator::runTask(
    Operations operations,
    const TaskAction action,
    QString root_path,
    const std::int64_t clock_offset_seconds,
    QString id,
    const quint64 request_id
) {
    TaskResult result;
    result.request_id = request_id;
    result.action = action;
    try {
        switch (action) {
        case TaskAction::Refresh:
            break;
        case TaskAction::AddOrRescan:
            static_cast<void>(operations.add_or_rescan(root_path, clock_offset_seconds));
            break;
        case TaskAction::Remove:
            static_cast<void>(operations.remove(id));
            break;
        }
        result.libraries = operations.libraries();
        result.has_snapshot = true;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        try {
            result.libraries = operations.libraries();
            result.has_snapshot = true;
        } catch (const std::exception&) {
            // Preserve the operation failure; a broken refresh is secondary.
        }
    }
    return result;
}

void ReviewLocationReferenceCoordinator::startTask(
    const TaskAction action,
    const QString& root_path,
    const std::int64_t clock_offset_seconds,
    const QString& id
) {
    task_running_ = true;
    error_text_.clear();
    active_request_id_ = ++request_id_;
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run(
        runTask,
        operations_,
        action,
        root_path,
        clock_offset_seconds,
        id,
        active_request_id_
    ));
}

void ReviewLocationReferenceCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool current = result.request_id == active_request_id_;
    if (current && result.has_snapshot) {
        libraries_ = std::move(result.libraries);
    }
    if (current) {
        error_text_ = result.error;
        if (result.error.isEmpty() && result.action != TaskAction::Refresh) {
            emit anchorsChanged();
        }
    }
    emit stateChanged();
    if (refresh_pending_) {
        refresh_pending_ = false;
        startTask(TaskAction::Refresh);
    }
}
