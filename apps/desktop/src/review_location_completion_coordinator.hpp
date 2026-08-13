#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Groups timestamped, unlocated Library photos into bounded capture events.
///
/// This is deliberately a read-only suggestion layer: it never resolves a
/// place, changes EXIF, or writes Catalog state. A caller must hand one group
/// to the existing coordinate-batch preview/accept flow before any location
/// override is persisted.
class ReviewLocationCompletionCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<BackendLibraryPhotoPage(
            const BackendLibraryPhotoFilter& filter,
            BackendLibraryPhotoOrder order,
            const BackendLibraryPhotoCursor& cursor,
            std::uint32_t limit
        )>
            page;
        std::function<QVector<BackendLocationReferenceAnchor>(std::int64_t, std::int64_t)> anchors;
    };

    explicit ReviewLocationCompletionCoordinator(Operations operations, QObject* parent = nullptr);
    ~ReviewLocationCompletionCoordinator() override;

    [[nodiscard]] QVariantList groups() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool truncated() const noexcept;
    [[nodiscard]] QString errorText() const;

    void request(
        BackendLibraryPhotoFilter filter,
        std::int64_t capture_start_unix_seconds,
        std::int64_t capture_end_unix_seconds
    );
    void invalidate();

  signals:
    void stateChanged();

  private:
    struct TaskResult final {
        QVariantList groups;
        bool truncated = false;
        QString error;
        quint64 request_id = 0;
    };

    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        BackendLibraryPhotoFilter filter,
        std::int64_t capture_start_unix_seconds,
        std::int64_t capture_end_unix_seconds,
        quint64 request_id
    );
    void startTask();
    void finishTask();

    Operations operations_;
    BackendLibraryPhotoFilter requested_filter_;
    std::int64_t requested_capture_start_unix_seconds_ = 0;
    std::int64_t requested_capture_end_unix_seconds_ = 0;
    QVariantList groups_;
    QString error_text_;
    bool task_running_ = false;
    bool request_pending_ = false;
    bool truncated_ = false;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    QFutureWatcher<TaskResult> watcher_;
};
