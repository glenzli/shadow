#pragma once

#include "desktop_backend.hpp"
#include "review_decision_session.hpp"
#include "review_evidence_session.hpp"
#include "review_model.hpp"

#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
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
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)

public:
    explicit ReviewController(
        std::shared_ptr<DesktopBackend> backend,
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
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;

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
    Q_INVOKABLE void undoLastDecision();

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
    void setStatusText(QString status);
    void updateScanStatus();
    void updateReadyStatus();
    void setComparisonStatusText(QString status);
    void setDecisionStatusText(QString status);
    void applyDecisionState(const BackendReviewDecisionState& state);

    std::shared_ptr<DesktopBackend> backend_;
    QString folder_path_;
    QString status_text_ = QStringLiteral("Choose a folder to build your Review library");
    QString comparison_status_text_ = QStringLiteral(
        "Explicit choices are recorded as evidence; no preference model is active"
    );
    QString decision_status_text_ = QStringLiteral(
        "Flags and stars are explicit local library decisions"
    );
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
    ReviewEvidenceSession evidence_session_;
    ReviewDecisionSession decision_session_;
    QFutureWatcher<ScanTaskResult> scan_watcher_;
    QFutureWatcher<PageTaskResult> page_watcher_;
    QFutureWatcher<ReviewEvidenceTaskResult> evidence_watcher_;
    QFutureWatcher<ReviewDecisionTaskResult> decision_watcher_;
};
