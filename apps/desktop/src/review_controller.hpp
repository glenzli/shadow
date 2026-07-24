#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"
#include "review_decision_session.hpp"
#include "review_evidence_session.hpp"
#include "review_filter_model.hpp"
#include "review_model.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>

class QSettings;

struct ScanTaskResult final {
    BackendScanReport report;
    QString error;
    quint64 generation = 0;
};

enum class PageTaskKind : std::uint8_t {
    InitialReset,
    StreamingPrefix,
    FinalReset,
    Append,
};

struct PageTaskResult final {
    BackendReviewPage page;
    QString error;
    quint64 library_generation = 0;
    quint64 request_id = 0;
    PageTaskKind kind = PageTaskKind::InitialReset;
};

enum class ReviewEvidenceTaskKind : std::uint8_t {
    Record,
    Forget,
};

struct ReviewEvidenceTaskResult final {
    BackendFeedbackReceipt feedback;
    BackendForgetReceipt forget;
    QString error;
    ReviewEvidenceTaskKind kind = ReviewEvidenceTaskKind::Record;
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
    [[nodiscard]] int filteredItemCount() const noexcept;
    [[nodiscard]] QVariantList sharedGradeNodes() const;
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;

    void setFilterFlag(const QString& filter);
    void setFilterMinimumRating(int rating);
    void setFilterColorLabel(const QString& color_label);
    void setFilterEditState(const QString& edit_state);

    Q_INVOKABLE void scanFolder(const QUrl& folder_url);
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE void loadMore();
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
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void refreshVisibleLibrary();
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
    void filtersChanged();
    void sharedGradeNodesChanged();
    void decisionUndone();

private:
    void finishScan();
    void finishPage();
    void pollScanProgress();
    void finishEvidenceTask();
    void finishDecisionTask();
    void startPage(PageTaskKind kind);
    void requestFinalPageRefresh();
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
    void setComparisonStatusMessage(LocalizedUiMessage status);
    void setDecisionStatusMessage(LocalizedUiMessage status);
    void applyDecisionState(const BackendReviewDecisionState& state);
    void persistColorLabels();

    std::shared_ptr<DesktopBackend> backend_;
    QString folder_path_;
  LocalizedUiMessage status_message_{
      "ReviewController",
      QT_TRANSLATE_NOOP("ReviewController",
                        "Choose a folder to build your Review library"),
  };
  LocalizedUiMessage comparison_status_message_{
      "ReviewController",
      QT_TRANSLATE_NOOP("ReviewController",
                        "Explicit choices are recorded as evidence; no "
                        "preference model is active"),
  };
  LocalizedUiMessage decision_status_message_{
      "ReviewController",
      QT_TRANSLATE_NOOP("ReviewController",
                        "Flags and stars are explicit local library decisions"
    ),
  };
    QString next_cursor_path_;
    QString next_cursor_representation_id_;
    quint64 library_generation_ = 1;
    quint64 scan_generation_ = 0;
    quint64 page_request_id_ = 0;
    quint64 active_page_request_id_ = 0;
    quint64 scan_update_sequence_ = 0;
    quint64 total_items_ = 0;
    quint64 files_seen_ = 0;
    quint64 supported_files_ = 0;
    quint64 inserted_files_ = 0;
    quint64 unchanged_files_ = 0;
    quint64 revalidation_files_ = 0;
    quint64 decode_queued_ = 0;
    quint64 decode_completed_ = 0;
    quint64 decode_hard_failures_ = 0;
    quint64 preview_failures_ = 0;
    quint64 decode_cancelled_ = 0;
    quint64 skipped_files_ = 0;
    quint64 issue_count_ = 0;
    quint64 next_stream_refresh_at_ = 1;
    qint64 last_stream_refresh_ms_ = -1;
    BackendScanPhase scan_phase_ = BackendScanPhase::Idle;
    QString scan_terminal_error_;
    bool scan_running_ = false;
    bool page_running_ = false;
    bool page_reset_running_ = false;
    bool final_page_refresh_pending_ = false;
    bool terminal_refresh_active_ = false;
    bool scan_terminal_cancelled_ = false;
    bool has_more_ = false;
    QElapsedTimer scan_clock_;
    QTimer scan_progress_timer_;
    ReviewModel model_;
    ReviewFilterModel filtered_model_;
    std::unique_ptr<QSettings> settings_;
    ReviewEvidenceSession evidence_session_;
    ReviewDecisionSession decision_session_;
    QVector<BackendSharedGradeNode> shared_grade_nodes_;
    QFutureWatcher<ScanTaskResult> scan_watcher_;
    QFutureWatcher<PageTaskResult> page_watcher_;
    QFutureWatcher<ReviewEvidenceTaskResult> evidence_watcher_;
    QFutureWatcher<ReviewDecisionTaskResult> decision_watcher_;
};
