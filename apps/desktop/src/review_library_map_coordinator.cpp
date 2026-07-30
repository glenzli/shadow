#include "review_library_map_coordinator.hpp"

#include <QDebug>
#include <QtConcurrentRun>

#include <stdexcept>
#include <utility>

ReviewLibraryMapCoordinator::ReviewLibraryMapCoordinator(
    Operations operations,
    QObject* const parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.snapshot) {
        throw std::invalid_argument("the Review Library map snapshot operation is required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLibraryMapCoordinator::finishTask
    );
}

ReviewLibraryMapCoordinator::~ReviewLibraryMapCoordinator() {
    watcher_.waitForFinished();
}

QVariantList ReviewLibraryMapCoordinator::clusters() const {
    return clusters_;
}

qulonglong ReviewLibraryMapCoordinator::photoCount() const noexcept {
    return static_cast<qulonglong>(snapshot_.photo_count);
}

bool ReviewLibraryMapCoordinator::busy() const noexcept {
    return task_running_;
}

bool ReviewLibraryMapCoordinator::failed() const noexcept {
    return failed_;
}

void ReviewLibraryMapCoordinator::request(
    BackendLibraryPhotoFilter filter,
    const BackendLibraryMapViewport viewport,
    const BackendLibraryMapGrid grid
) {
    requested_filter_ = std::move(filter);
    requested_viewport_ = viewport;
    requested_grid_ = grid;
    ++request_id_;
    if (task_running_) {
        request_pending_ = true;
        return;
    }
    startTask();
}

ReviewLibraryMapCoordinator::TaskResult ReviewLibraryMapCoordinator::runTask(
    Operations operations,
    BackendLibraryPhotoFilter filter,
    const BackendLibraryMapViewport viewport,
    const BackendLibraryMapGrid grid,
    const quint64 request_id
) {
    TaskResult result;
    result.request_id = request_id;
    try {
        result.snapshot = operations.snapshot(filter, viewport, grid);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

QVariantList ReviewLibraryMapCoordinator::variants(const BackendLibraryMapSnapshot& snapshot) {
    QVariantList values;
    values.reserve(snapshot.clusters.size());
    for (const auto& cluster : snapshot.clusters) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("cellX"), cluster.cell_x},
                {QStringLiteral("cellY"), cluster.cell_y},
                {
                    QStringLiteral("latitude"),
                    static_cast<double>(cluster.latitude_e7) / 10'000'000.0,
                },
                {
                    QStringLiteral("longitude"),
                    static_cast<double>(cluster.longitude_e7) / 10'000'000.0,
                },
                {
                    QStringLiteral("photoCount"),
                    QVariant::fromValue(static_cast<qulonglong>(cluster.photo_count)),
                },
                {QStringLiteral("photoId"), cluster.photo_id},
                {
                    QStringLiteral("representationId"),
                    cluster.representation_id,
                },
                {QStringLiteral("title"), cluster.title},
                {QStringLiteral("sourcePath"), cluster.source_path},
            }
        );
    }
    return values;
}

void ReviewLibraryMapCoordinator::startTask() {
    task_running_ = true;
    failed_ = false;
    active_request_id_ = request_id_;
    emit stateChanged();
    watcher_.setFuture(
        QtConcurrent::run(
            runTask,
            operations_,
            requested_filter_,
            requested_viewport_,
            requested_grid_,
            active_request_id_
        )
    );
}

void ReviewLibraryMapCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool accepted = result.request_id == request_id_;
    if (accepted && result.error.isEmpty()) {
        snapshot_ = std::move(result.snapshot);
        clusters_ = variants(snapshot_);
        failed_ = false;
    } else if (accepted) {
        failed_ = true;
        qWarning().noquote() << "Library map snapshot failed:" << result.error;
    }

    if (request_pending_ || !accepted) {
        request_pending_ = false;
        startTask();
        return;
    }
    emit stateChanged();
}
