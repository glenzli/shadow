#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <functional>

/// Owns coalesced, generation-safe spatial overlay queries for the Library map.
///
/// Tile retrieval remains a Qt Location/provider concern. This coordinator only
/// asks the local catalog for bounded clusters and never blocks the UI thread.
class ReviewLibraryMapCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<BackendLibraryMapSnapshot(
            const BackendLibraryPhotoFilter& filter,
            const BackendLibraryMapViewport& viewport,
            const BackendLibraryMapGrid& grid
        )>
            snapshot;
    };

    explicit ReviewLibraryMapCoordinator(Operations operations, QObject* parent = nullptr);
    ~ReviewLibraryMapCoordinator() override;

    [[nodiscard]] QVariantList clusters() const;
    [[nodiscard]] qulonglong photoCount() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

    void request(
        BackendLibraryPhotoFilter filter,
        BackendLibraryMapViewport viewport,
        BackendLibraryMapGrid grid
    );

  signals:
    void stateChanged();

  private:
    struct TaskResult final {
        BackendLibraryMapSnapshot snapshot;
        QString error;
        quint64 request_id = 0;
    };

    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        BackendLibraryPhotoFilter filter,
        BackendLibraryMapViewport viewport,
        BackendLibraryMapGrid grid,
        quint64 request_id
    );
    [[nodiscard]] static QVariantList variants(const BackendLibraryMapSnapshot& snapshot);

    void startTask();
    void finishTask();

    Operations operations_;
    BackendLibraryPhotoFilter requested_filter_;
    BackendLibraryMapViewport requested_viewport_;
    BackendLibraryMapGrid requested_grid_;
    BackendLibraryMapSnapshot snapshot_;
    QVariantList clusters_;
    bool task_running_ = false;
    bool request_pending_ = false;
    bool failed_ = false;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    QFutureWatcher<TaskResult> watcher_;
};
