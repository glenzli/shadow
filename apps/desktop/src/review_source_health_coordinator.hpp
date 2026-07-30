#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns Review's complete Library source-health review lifecycle.
///
/// A completed folder scan is only an external refresh trigger. This owner
/// independently serializes source-health reads, missing-location paging, and
/// exact user-selected relinks; rejects stale review pages; publishes the
/// stable QML projections and localized terminal status; and waits for every
/// worker before destruction.
class ReviewSourceHealthCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<QVector<BackendLibrarySourceHealth>()> source_health;
        std::function<bool(const QString& source_id)> remove_source;
        std::function<BackendMissingSourceLocationPage(
            const QString& scan_session_id,
            const QString& after_location_id,
            std::uint32_t limit
        )>
            missing_locations;
        std::function<BackendVerifiedSourceRelinkReceipt(
            const QString& scan_session_id,
            const QString& location_id,
            const QString& candidate_path
        )>
            relink;
    };

    explicit ReviewSourceHealthCoordinator(Operations operations, QObject* parent = nullptr);
    ~ReviewSourceHealthCoordinator() override;

    [[nodiscard]] QVariantList sourceHealth() const;
    [[nodiscard]] bool sourceHealthBusy() const noexcept;
    [[nodiscard]] bool removeSourceBusy() const noexcept;
    [[nodiscard]] QVariantList missingLocations() const;
    [[nodiscard]] QString missingLocationScanId() const;
    [[nodiscard]] bool missingLocationsBusy() const noexcept;
    [[nodiscard]] bool missingLocationsHasMore() const noexcept;
    [[nodiscard]] bool relinkBusy() const noexcept;
    [[nodiscard]] QString relinkStatusText() const;
    [[nodiscard]] LocalizedUiMessage globalStatusMessage() const;

    void refreshSourceHealth();
    void removeSource(const QString& source_id, const QString& source_path);
    void openMissingLocationReview(const QString& scan_session_id);
    void closeMissingLocationReview();
    void loadMoreMissingLocations();
    void relinkMissingLocation(const QString& location_id, const QUrl& candidate_url);
    void retranslateUi();

  signals:
    void sourceHealthChanged();
    void libraryVisibilityChanged();
    void missingLocationReviewChanged();
    void globalStatusMessageChanged();

  private:
    struct SourceHealthTaskResult final {
        QVector<BackendLibrarySourceHealth> sources;
        QString error;
        quint64 request_id = 0;
    };

    struct MissingLocationTaskResult final {
        BackendMissingSourceLocationPage page;
        QString error;
        QString scan_session_id;
        quint64 request_id = 0;
        bool append = false;
    };

    struct RemoveSourceTaskResult final {
        bool removed = false;
        QString error;
        QString source_id;
        QString source_path;
        quint64 request_id = 0;
    };

    struct RelinkTaskResult final {
        BackendVerifiedSourceRelinkReceipt receipt;
        QString error;
        QString location_id;
        quint64 request_id = 0;
    };

    [[nodiscard]] static SourceHealthTaskResult runSourceHealthTask(
        std::function<QVector<BackendLibrarySourceHealth>()> operation,
        quint64 request_id
    );
    [[nodiscard]] static MissingLocationTaskResult runMissingLocationTask(
        std::function<BackendMissingSourceLocationPage(
            const QString& scan_session_id,
            const QString& after_location_id,
            std::uint32_t limit
        )> operation,
        QString scan_session_id,
        QString after_location_id,
        quint64 request_id,
        bool append
    );
    [[nodiscard]] static RemoveSourceTaskResult runRemoveSourceTask(
        std::function<bool(const QString& source_id)> operation,
        QString source_id,
        QString source_path,
        quint64 request_id
    );
    [[nodiscard]] static RelinkTaskResult runRelinkTask(
        std::function<BackendVerifiedSourceRelinkReceipt(
            const QString& scan_session_id,
            const QString& location_id,
            const QString& candidate_path
        )> operation,
        QString scan_session_id,
        QString location_id,
        QString candidate_path,
        quint64 request_id
    );

    void startSourceHealthTask();
    void startRemoveSourceTask(const QString& source_id, const QString& source_path);
    void startMissingLocationTask(bool append);
    void startRelinkTask(const QString& location_id, const QString& candidate_path);
    void finishSourceHealthTask();
    void finishRemoveSourceTask();
    void finishMissingLocationTask();
    void finishRelinkTask();
    void publishGlobalStatus(LocalizedUiMessage status);

    Operations operations_;
    bool source_health_running_ = false;
    bool source_health_refresh_pending_ = false;
    quint64 source_health_request_id_ = 0;
    quint64 active_source_health_request_id_ = 0;
    QVector<BackendLibrarySourceHealth> source_health_;
    bool remove_source_running_ = false;
    quint64 remove_source_request_id_ = 0;
    quint64 active_remove_source_request_id_ = 0;
    bool missing_locations_running_ = false;
    bool missing_locations_refresh_pending_ = false;
    quint64 missing_locations_request_id_ = 0;
    quint64 active_missing_locations_request_id_ = 0;
    QString missing_location_scan_id_;
    QString missing_location_next_cursor_;
    QVector<BackendMissingSourceLocation> missing_locations_;
    bool missing_locations_has_more_ = false;
    bool relink_running_ = false;
    quint64 relink_request_id_ = 0;
    quint64 active_relink_request_id_ = 0;
    LocalizedUiMessage relink_status_message_;
    LocalizedUiMessage global_status_message_;
    QFutureWatcher<SourceHealthTaskResult> source_health_watcher_;
    QFutureWatcher<RemoveSourceTaskResult> remove_source_watcher_;
    QFutureWatcher<MissingLocationTaskResult> missing_locations_watcher_;
    QFutureWatcher<RelinkTaskResult> relink_watcher_;
};
