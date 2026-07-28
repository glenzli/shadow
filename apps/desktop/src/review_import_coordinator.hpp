#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <functional>

/// Owns one folder-import lifecycle from admission through terminal refresh.
///
/// The coordinator assigns the scan identity, runs the blocking import away
/// from the UI thread, polls monotonic progress, applies cooperative
/// cancellation, paces live Library refreshes, publishes localized status, and
/// waits for the worker during destruction. ReviewController remains the
/// cross-workflow arbiter and owns only the resulting Library page refresh.
class ReviewImportCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<void(quint64 scan_id)> begin;
        std::function<BackendScanReport(
            const QString& folder_path,
            quint64 scan_id
        )> scan;
        std::function<BackendScanProgress(quint64 scan_id)> progress;
        std::function<bool(quint64 scan_id)> cancel;
    };

    explicit ReviewImportCoordinator(
        Operations operations,
        QObject* parent = nullptr
    );
    ~ReviewImportCoordinator() override;

    [[nodiscard]] bool scanning() const noexcept;
    [[nodiscard]] QString folderPath() const;
    [[nodiscard]] QVariantMap progress() const;
    [[nodiscard]] LocalizedUiMessage statusMessage() const;
    [[nodiscard]] LocalizedUiMessage refreshingStatusMessage() const;
    [[nodiscard]] LocalizedUiMessage readyStatusMessage(
        quint64 total_items,
        int visible_items,
        bool loading_more
    ) const;

    /// Starts an admitted import. Rejected or invalid requests change no scan
    /// identity; user-facing local-path and backend failures are published.
    [[nodiscard]] bool start(const QUrl& folder_url, bool admitted);
    [[nodiscard]] bool cancel();

    /// Consumes one paced live-refresh opportunity for the latest progress.
    /// The caller supplies only current presentation facts; all pacing state
    /// remains owned by this lifecycle.
    [[nodiscard]] bool takeStreamRefreshRequest(
        int visible_items,
        bool page_running
    );

    void retranslateUi();

signals:
    void runningChanged();
    void folderPathChanged();
    void progressChanged();
    void statusMessageChanged();
    void terminalRefreshRequested();

private:
    struct TaskResult final {
        BackendScanReport report;
        QString error;
        quint64 generation = 0;
    };

    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        QString folder_path,
        quint64 generation
    );
    [[nodiscard]] quint64 cataloguedFiles() const noexcept;
    void pollProgress();
    void finishTask();
    void publishStatus(LocalizedUiMessage status);
    void updateActiveStatus();
    void resetProgress();
    void applyProgress(const BackendScanProgress& progress);

    Operations operations_;
    QString folder_path_;
    LocalizedUiMessage status_message_;
    quint64 generation_ = 0;
    quint64 update_sequence_ = 0;
    quint64 files_seen_ = 0;
    quint64 supported_files_ = 0;
    quint64 inserted_files_ = 0;
    quint64 unchanged_files_ = 0;
    quint64 revalidation_files_ = 0;
    quint64 decode_queued_ = 0;
    quint64 preview_artifacts_ready_ = 0;
    quint64 decode_completed_ = 0;
    quint64 decode_hard_failures_ = 0;
    quint64 preview_failures_ = 0;
    quint64 decode_cancelled_ = 0;
    quint64 skipped_files_ = 0;
    quint64 issue_count_ = 0;
    quint64 next_stream_refresh_at_ = 1;
    quint64 last_stream_visual_refresh_at_ = 0;
    qint64 last_stream_refresh_ms_ = -1;
    BackendScanPhase phase_ = BackendScanPhase::Idle;
    QString terminal_error_;
    bool running_ = false;
    bool terminal_cancelled_ = false;
    QElapsedTimer clock_;
    QTimer progress_timer_;
    QFutureWatcher<TaskResult> watcher_;
};
