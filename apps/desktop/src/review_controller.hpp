#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"
#include "review_comparison_coordinator.hpp"
#include "review_decision_session.hpp"
#include "review_filter_model.hpp"
#include "review_model.hpp"
#include "review_photo_inspection_coordinator.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>

struct ScanTaskResult final {
    BackendScanReport report;
    QString error;
    quint64 generation = 0;
};

enum class PageTaskKind : std::uint8_t {
    InitialReset,
    StreamingPrefix,
    Append,
};

struct PageTaskResult final {
    BackendLibraryPhotoPage page;
    QString error;
    quint64 library_generation = 0;
    quint64 request_id = 0;
    PageTaskKind kind = PageTaskKind::InitialReset;
};

struct CountTaskResult final {
    quint64 count = 0;
    QString error;
    quint64 library_generation = 0;
    quint64 request_id = 0;
};

/// Three independently bounded, catalog-side metadata facets fetched as one
/// worker result. The grid never waits on these aggregates to paginate.
struct LibraryFacetTaskResult final {
    BackendLibraryFacetPage capture_months;
    BackendLibraryFacetPage cameras;
    BackendLibraryFacetPage lenses;
    QString error;
    quint64 library_generation = 0;
    quint64 request_id = 0;
};

struct LibraryStateTaskResult final {
    BackendPhotoLibraryState state;
    QString error;
    QString requested_photo_id;
};

enum class LibraryAlbumTaskAction : std::uint8_t {
    Refresh,
    CreateManual,
    CreateSmart,
    Rename,
    Delete,
    AddPhotos,
    RemovePhotos,
};

struct LibraryAlbumTaskResult final {
    QVector<BackendLibraryAlbum> albums;
    QString error;
    quint64 request_id = 0;
    LibraryAlbumTaskAction action = LibraryAlbumTaskAction::Refresh;
    QString album_id;
    int affected_photo_count = 0;
    bool has_album_snapshot = false;
};

struct LibrarySourceHealthTaskResult final {
    QVector<BackendLibrarySourceHealth> sources;
    QString error;
    quint64 request_id = 0;
};

struct MissingSourceLocationTaskResult final {
    BackendMissingSourceLocationPage page;
    QString error;
    QString scan_session_id;
    quint64 request_id = 0;
    bool append = false;
};

struct MissingSourceRelinkTaskResult final {
    BackendVerifiedSourceRelinkReceipt receipt;
    QString error;
    QString location_id;
    quint64 request_id = 0;
};

struct ReviewDecisionTaskResult final {
    BackendReviewDecisionMutationReceipt receipt;
    BackendReviewDecisionState authoritative;
    QString error;
    QString refresh_error;
    bool has_authoritative = false;
    bool is_undo = false;
};

class ReviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY folderPathChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QVariantMap scanProgress READ scanProgress NOTIFY scanProgressChanged)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY itemCountChanged)
    Q_PROPERTY(
        QVariantMap photoInspection
        READ photoInspection
        NOTIFY photoInspectionChanged
    )
    Q_PROPERTY(
        bool photoInspectionBusy
        READ photoInspectionBusy
        NOTIFY photoInspectionChanged
    )
    Q_PROPERTY(
        bool photoInspectionFailed
        READ photoInspectionFailed
        NOTIFY photoInspectionChanged
    )
    Q_PROPERTY(
        bool comparisonBusy
        READ comparisonBusy
        NOTIFY comparisonStateChanged
    )
    Q_PROPERTY(
        bool canUndoComparison
        READ canUndoComparison
        NOTIFY comparisonStateChanged
    )
    Q_PROPERTY(
        int sessionEvidenceCount
        READ sessionEvidenceCount
        NOTIFY comparisonStateChanged
    )
    Q_PROPERTY(
        QString comparisonStatusText
        READ comparisonStatusText
        NOTIFY comparisonStatusTextChanged
    )
    Q_PROPERTY(bool decisionBusy READ decisionBusy NOTIFY decisionStateChanged)
    Q_PROPERTY(
        bool canUndoDecision
        READ canUndoDecision
        NOTIFY decisionStateChanged
    )
    Q_PROPERTY(
        QString decisionStatusText
        READ decisionStatusText
        NOTIFY decisionStatusTextChanged
    )
    Q_PROPERTY(
        QString filterFlag
        READ filterFlag
        WRITE setFilterFlag
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        int filterMinimumRating
        READ filterMinimumRating
        WRITE setFilterMinimumRating
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterColorLabel
        READ filterColorLabel
        WRITE setFilterColorLabel
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterEditState
        READ filterEditState
        WRITE setFilterEditState
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterLiked
        READ filterLiked
        WRITE setFilterLiked
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterCaptureMonth
        READ filterCaptureMonth
        WRITE setFilterCaptureMonth
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterCameraKey
        READ filterCameraKey
        WRITE setFilterCameraKey
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString filterLensKey
        READ filterLensKey
        WRITE setFilterLensKey
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QVariantList libraryCaptureMonthFacets
        READ libraryCaptureMonthFacets
        NOTIFY libraryFacetsChanged
    )
    Q_PROPERTY(
        QVariantList libraryCameraFacets
        READ libraryCameraFacets
        NOTIFY libraryFacetsChanged
    )
    Q_PROPERTY(
        QVariantList libraryLensFacets
        READ libraryLensFacets
        NOTIFY libraryFacetsChanged
    )
    Q_PROPERTY(
        bool libraryFacetsBusy
        READ libraryFacetsBusy
        NOTIFY libraryFacetsChanged
    )
    Q_PROPERTY(
        QString libraryAlbumId
        READ libraryAlbumId
        WRITE setLibraryAlbumId
        NOTIFY libraryAlbumChanged
    )
    Q_PROPERTY(
        QVariantList libraryAlbums
        READ libraryAlbums
        NOTIFY libraryAlbumsChanged
    )
    Q_PROPERTY(
        bool libraryAlbumsBusy
        READ libraryAlbumsBusy
        NOTIFY libraryAlbumsChanged
    )
    Q_PROPERTY(
        QVariantList librarySourceHealth
        READ librarySourceHealth
        NOTIFY librarySourceHealthChanged
    )
    Q_PROPERTY(
        bool librarySourceHealthBusy
        READ librarySourceHealthBusy
        NOTIFY librarySourceHealthChanged
    )
    Q_PROPERTY(
        QVariantList missingSourceLocations
        READ missingSourceLocations
        NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        QString missingSourceLocationScanId
        READ missingSourceLocationScanId
        NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        bool missingSourceLocationsBusy
        READ missingSourceLocationsBusy
        NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        bool missingSourceLocationsHasMore
        READ missingSourceLocationsHasMore
        NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        bool sourceRelinkBusy
        READ sourceRelinkBusy
        NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        QString sourceRelinkStatusText
        READ sourceRelinkStatusText
        NOTIFY missingSourceLocationReviewChanged
    )
    Q_PROPERTY(
        int filteredItemCount
        READ filteredItemCount
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QVariantList sharedGradeNodes
        READ sharedGradeNodes
        NOTIFY sharedGradeNodesChanged
    )
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)

public:
    explicit ReviewController(
        std::shared_ptr<DesktopBackend> backend,
        const QString& isolated_settings_file = {},
        QObject* parent = nullptr
    );
    ~ReviewController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool scanning() const noexcept;
    [[nodiscard]] bool refreshing() const noexcept;
    [[nodiscard]] bool loadingMore() const noexcept;
    [[nodiscard]] bool hasMore() const noexcept;
    [[nodiscard]] QString folderPath() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QVariantMap scanProgress() const;
    [[nodiscard]] int itemCount() const;
    [[nodiscard]] QVariantMap photoInspection() const;
    [[nodiscard]] bool photoInspectionBusy() const noexcept;
    [[nodiscard]] bool photoInspectionFailed() const noexcept;
    [[nodiscard]] bool comparisonBusy() const noexcept;
    [[nodiscard]] bool canUndoComparison() const noexcept;
    [[nodiscard]] int sessionEvidenceCount() const noexcept;
    [[nodiscard]] QString comparisonStatusText() const;
    [[nodiscard]] bool decisionBusy() const noexcept;
    [[nodiscard]] bool canUndoDecision() const;
    [[nodiscard]] QString decisionStatusText() const;
    [[nodiscard]] QString filterFlag() const;
    [[nodiscard]] int filterMinimumRating() const noexcept;
    [[nodiscard]] QString filterColorLabel() const;
    [[nodiscard]] QString filterEditState() const;
    [[nodiscard]] QString filterLiked() const;
    [[nodiscard]] QString filterCaptureMonth() const;
    [[nodiscard]] QString filterCameraKey() const;
    [[nodiscard]] QString filterLensKey() const;
    [[nodiscard]] QVariantList libraryCaptureMonthFacets() const;
    [[nodiscard]] QVariantList libraryCameraFacets() const;
    [[nodiscard]] QVariantList libraryLensFacets() const;
    [[nodiscard]] bool libraryFacetsBusy() const noexcept;
    [[nodiscard]] QString libraryAlbumId() const;
    [[nodiscard]] QVariantList libraryAlbums() const;
    [[nodiscard]] bool libraryAlbumsBusy() const noexcept;
    [[nodiscard]] QVariantList librarySourceHealth() const;
    [[nodiscard]] bool librarySourceHealthBusy() const noexcept;
    [[nodiscard]] QVariantList missingSourceLocations() const;
    [[nodiscard]] QString missingSourceLocationScanId() const;
    [[nodiscard]] bool missingSourceLocationsBusy() const noexcept;
    [[nodiscard]] bool missingSourceLocationsHasMore() const noexcept;
    [[nodiscard]] bool sourceRelinkBusy() const noexcept;
    [[nodiscard]] QString sourceRelinkStatusText() const;
    [[nodiscard]] int filteredItemCount() const noexcept;
    [[nodiscard]] QVariantList sharedGradeNodes() const;
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;

    void setFilterFlag(const QString& filter);
    void setFilterMinimumRating(int rating);
    void setFilterColorLabel(const QString& color_label);
    void setFilterEditState(const QString& edit_state);
    void setFilterLiked(const QString& liked);
    void setFilterCaptureMonth(const QString& capture_month);
    void setFilterCameraKey(const QString& camera_key);
    void setFilterLensKey(const QString& lens_key);
    void setLibraryAlbumId(const QString& album_id);

    Q_INVOKABLE void scanFolder(const QUrl& folder_url);
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void requestPhotoInspection(
        const QString& photo_id,
        const QString& representation_id
    );
    Q_INVOKABLE void retryPhotoInspection();
    Q_INVOKABLE void clearPhotoInspection();
    /// Returns the inclusive, currently filtered Library range between two
    /// presentation identities. This keeps Shift selection stable even when a
    /// justified grid has virtualized most of its delegates.
    Q_INVOKABLE QVariantList selectionRangeTargets(
        const QString& anchor_photo_id,
        const QString& anchor_representation_id,
        const QString& photo_id,
        const QString& representation_id
    ) const;
    Q_INVOKABLE QVariantMap prepareComparison(
        const QString& left_visual_handle,
        const QString& right_visual_handle
    );
    Q_INVOKABLE bool confirmComparisonReady(
        const QString& presentation_id,
        const QString& left_request_ticket,
        const QString& right_request_ticket
    );
    Q_INVOKABLE void cancelComparison(const QString& presentation_id);
    Q_INVOKABLE void recordComparison(const QString& presentation_id, int outcome);
    Q_INVOKABLE void undoLastComparison();
    Q_INVOKABLE void setPhotoFlag(const QString& photo_id, const QString& flag);
    Q_INVOKABLE void setPhotoRating(const QString& photo_id, int rating);
    Q_INVOKABLE void setPhotoColorLabel(
        const QString& photo_id,
        const QString& color_label
    );
    Q_INVOKABLE void setPhotoLiked(const QString& photo_id, bool liked);
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void refreshVisibleLibrary();
    Q_INVOKABLE void refreshLibraryFacets();
    Q_INVOKABLE void setLibraryFacet(const QString& kind, const QString& key);
    Q_INVOKABLE void clearLibraryFacet(const QString& kind);
    Q_INVOKABLE void refreshLibraryAlbums();
    Q_INVOKABLE void refreshLibrarySourceHealth();
    Q_INVOKABLE void openMissingSourceLocationReview(const QString& scan_session_id);
    Q_INVOKABLE void closeMissingSourceLocationReview();
    Q_INVOKABLE void loadMoreMissingSourceLocations();
    Q_INVOKABLE void relinkMissingSourceLocation(
        const QString& location_id,
        const QUrl& candidate_url
    );
    Q_INVOKABLE void createManualLibraryAlbum(const QString& name);
    Q_INVOKABLE void createSmartLibraryAlbum(const QString& name);
    Q_INVOKABLE void renameLibraryAlbum(
        const QString& album_id,
        const QString& name
    );
    Q_INVOKABLE void deleteLibraryAlbum(const QString& album_id);
    Q_INVOKABLE void addPhotosToManualLibraryAlbum(
        const QString& album_id,
        const QVariantList& targets
    );
    Q_INVOKABLE void removePhotosFromManualLibraryAlbum(
        const QString& album_id,
        const QVariantList& targets
    );
    Q_INVOKABLE void refreshSharedGradeNodes();
    Q_INVOKABLE QVariantMap applySharedGradeNode(
        const QString& layer_id,
        const QVariantList& targets
    );
    Q_INVOKABLE void undoLastDecision();
  Q_INVOKABLE void retranslateUi();

signals:
    void busyChanged();
    void scanningChanged();
    void refreshingChanged();
    void scanProgressChanged();
    void loadingMoreChanged();
    void hasMoreChanged();
    void folderPathChanged();
    void statusTextChanged();
    void itemCountChanged();
    void photoInspectionChanged();
    void comparisonStateChanged();
    void comparisonStatusTextChanged();
    void comparisonRecorded();
    void comparisonForgotten();
    void decisionStateChanged();
    void decisionStatusTextChanged();
    void decisionChanged(
        const QString& photoId,
        qulonglong headSequence,
        const QString& flag,
        int rating
    );
    void colorLabelChanged(const QString& photoId, const QString& colorLabel);
    void likedChanged(const QString& photoId, bool liked);
    void filtersChanged();
    void libraryAlbumChanged();
    void libraryAlbumsChanged();
    void libraryFacetsChanged();
    void librarySourceHealthChanged();
    void missingSourceLocationReviewChanged();
    void sharedGradeNodesChanged();
    void decisionUndone();

private:
    void finishScan();
    void finishPage();
    void finishCount();
    void finishLibraryFacetsTask();
    void finishLibraryStateTask();
    void finishLibraryAlbumsTask();
    void finishLibrarySourceHealthTask();
    void finishMissingSourceLocationTask();
    void finishMissingSourceRelinkTask();
    void pollScanProgress();
    void finishDecisionTask();
    void startPage(PageTaskKind kind);
    void requestLibraryReset();
    void scheduleFilterQuery();
    void beginFilteredLibraryQuery();
    void startCountQuery();
    void startLibraryFacetsTask();
    [[nodiscard]] BackendLibraryPhotoFilter currentLibraryFilter() const;
    void startLibraryStateMutation(
        const QString& photo_id,
        bool liked,
        const QString& color_label
    );
    void startLibraryAlbumsTask(
        LibraryAlbumTaskAction action,
        const QString& name = {},
        const QString& album_id = {},
        const QStringList& photo_ids = {}
    );
    void startLibrarySourceHealthTask();
    void startMissingSourceLocationTask(bool append);
    void startMissingSourceRelinkTask(const QString& location_id, const QString& candidate_path);
    void startDecisionMutation(const ReviewDecisionMutationRequest& request);
    void emitWorkStateChanges(
        bool old_busy,
        bool old_loading_more,
        bool old_refreshing
    );
    void setHasMore(bool has_more);
    bool eventFilter(QObject *watched, QEvent *event) override;
    void setStatusMessage(LocalizedUiMessage status);
    void updateScanStatus();
    void updateReadyStatus();
    void setDecisionStatusMessage(LocalizedUiMessage status);
    void applyDecisionState(const BackendReviewDecisionState& state);

    std::shared_ptr<DesktopBackend> backend_;
    ReviewPhotoInspectionCoordinator photo_inspection_coordinator_;
    QString folder_path_;
  LocalizedUiMessage status_message_{
      "ReviewController",
      QT_TRANSLATE_NOOP("ReviewController",
                        "Choose a folder to build your Review library"),
  };
  LocalizedUiMessage decision_status_message_{
      "ReviewController",
      QT_TRANSLATE_NOOP("ReviewController",
                        "Flags and stars are explicit local library decisions"
    ),
  };
    BackendLibraryPhotoCursor next_cursor_;
    quint64 library_generation_ = 1;
    quint64 scan_generation_ = 0;
    quint64 page_request_id_ = 0;
    quint64 active_page_request_id_ = 0;
    quint64 count_request_id_ = 0;
    quint64 active_count_request_id_ = 0;
    quint64 scan_update_sequence_ = 0;
    quint64 total_items_ = 0;
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
    BackendScanPhase scan_phase_ = BackendScanPhase::Idle;
    QString scan_terminal_error_;
    bool scan_running_ = false;
    bool page_running_ = false;
    bool page_reset_running_ = false;
    bool library_reset_pending_ = false;
    bool count_running_ = false;
    bool count_query_pending_ = false;
    bool terminal_refresh_active_ = false;
    bool scan_terminal_cancelled_ = false;
    bool has_more_ = false;
    bool library_state_mutation_running_ = false;
    bool library_albums_task_running_ = false;
    bool library_albums_refresh_pending_ = false;
    quint64 library_albums_request_id_ = 0;
    quint64 active_library_albums_request_id_ = 0;
    QString library_album_id_;
    QVector<BackendLibraryAlbum> library_albums_;
    bool library_facets_task_running_ = false;
    bool library_facets_refresh_pending_ = false;
    quint64 library_facets_request_id_ = 0;
    quint64 active_library_facets_request_id_ = 0;
    BackendLibraryFacetPage library_capture_month_facets_;
    BackendLibraryFacetPage library_camera_facets_;
    BackendLibraryFacetPage library_lens_facets_;
    bool library_source_health_task_running_ = false;
    bool library_source_health_refresh_pending_ = false;
    quint64 library_source_health_request_id_ = 0;
    quint64 active_library_source_health_request_id_ = 0;
    QVector<BackendLibrarySourceHealth> library_source_health_;
    bool missing_source_locations_task_running_ = false;
    bool missing_source_locations_refresh_pending_ = false;
    quint64 missing_source_locations_request_id_ = 0;
    quint64 active_missing_source_locations_request_id_ = 0;
    QString missing_source_location_scan_id_;
    QString missing_source_location_next_cursor_;
    QVector<BackendMissingSourceLocation> missing_source_locations_;
    bool missing_source_locations_has_more_ = false;
    bool source_relink_task_running_ = false;
    quint64 source_relink_request_id_ = 0;
    quint64 active_source_relink_request_id_ = 0;
    QString source_relink_status_text_;
    QElapsedTimer scan_clock_;
    QTimer scan_progress_timer_;
    QTimer filter_debounce_timer_;
    ReviewModel model_;
    ReviewFilterModel filtered_model_;
    ReviewComparisonCoordinator comparison_coordinator_;
    ReviewDecisionSession decision_session_;
    QVector<BackendSharedGradeNode> shared_grade_nodes_;
    QFutureWatcher<ScanTaskResult> scan_watcher_;
    QFutureWatcher<PageTaskResult> page_watcher_;
    QFutureWatcher<CountTaskResult> count_watcher_;
    QFutureWatcher<LibraryFacetTaskResult> library_facets_watcher_;
    QFutureWatcher<LibraryStateTaskResult> library_state_watcher_;
    QFutureWatcher<LibraryAlbumTaskResult> library_albums_watcher_;
    QFutureWatcher<LibrarySourceHealthTaskResult> library_source_health_watcher_;
    QFutureWatcher<MissingSourceLocationTaskResult> missing_source_locations_watcher_;
    QFutureWatcher<MissingSourceRelinkTaskResult> source_relink_watcher_;
    QFutureWatcher<ReviewDecisionTaskResult> decision_watcher_;
};
