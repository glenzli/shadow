#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns the asynchronous lifecycle of read-only location-reference folders.
///
/// Reference folders supply only capture-time/GPS evidence to the location
/// completion workflow. They remain outside the primary Library and are never
/// a source of new photo records or edits.
class ReviewLocationReferenceCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<QVector<BackendLocationReferenceLibrary>()> libraries;
        std::function<BackendLocationReferenceLibrary(const QString&, std::int64_t)> add_or_rescan;
        std::function<bool(const QString&)> remove;
    };

    explicit ReviewLocationReferenceCoordinator(Operations operations, QObject* parent = nullptr);
    ~ReviewLocationReferenceCoordinator() override;

    [[nodiscard]] QVariantList libraries() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString errorText() const;

    void refresh();
    void addOrRescan(const QString& root_path, std::int64_t clock_offset_seconds);
    void remove(const QString& id);

  signals:
    void stateChanged();
    void anchorsChanged();

  private:
    enum class TaskAction : std::uint8_t { Refresh, AddOrRescan, Remove };

    struct TaskResult final {
        QVector<BackendLocationReferenceLibrary> libraries;
        QString error;
        quint64 request_id = 0;
        TaskAction action = TaskAction::Refresh;
        bool has_snapshot = false;
    };

    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        TaskAction action,
        QString root_path,
        std::int64_t clock_offset_seconds,
        QString id,
        quint64 request_id
    );
    void startTask(
        TaskAction action,
        const QString& root_path = {},
        std::int64_t clock_offset_seconds = 0,
        const QString& id = {}
    );
    void finishTask();

    Operations operations_;
    QVector<BackendLocationReferenceLibrary> libraries_;
    QString error_text_;
    bool task_running_ = false;
    bool refresh_pending_ = false;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    QFutureWatcher<TaskResult> watcher_;
};
