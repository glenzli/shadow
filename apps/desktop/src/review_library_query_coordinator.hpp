#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"
#include "review_model.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include <functional>

/// Owns the generation-bound Review Library page and count projection.
///
/// One immutable active filter governs page, cursor, count, and model
/// generation. The coordinator serializes page work, coalesces debounced
/// resets, rejects stale results, validates continuation cursors, reconciles
/// snapshots into ReviewModel, and waits for both workers at destruction.
class ReviewLibraryQueryCoordinator final : public QObject {
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
        std::function<quint64(const BackendLibraryPhotoFilter& filter)> count;
    };

    using DecisionReconciler = std::function<void(BackendReviewDecisionState state)>;

    explicit ReviewLibraryQueryCoordinator(
        Operations operations,
        ReviewModel& model,
        QObject* parent = nullptr
    );
    ~ReviewLibraryQueryCoordinator() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool refreshing() const noexcept;
    [[nodiscard]] bool loadingMore() const noexcept;
    [[nodiscard]] bool pageRunning() const noexcept;
    [[nodiscard]] bool hasMore() const noexcept;
    [[nodiscard]] int itemCount() const noexcept;
    [[nodiscard]] quint64 totalItems() const noexcept;
    [[nodiscard]] quint64 generation() const noexcept;
    [[nodiscard]] LocalizedUiMessage statusMessage() const;

    void setDecisionReconciler(DecisionReconciler reconciler);
    void setScanRunning(bool running);
    void setDecisionBusy(bool busy);
    void clearForImportStart();
    void requestReset(BackendLibraryPhotoFilter filter, BackendLibraryPhotoOrder order);
    void scheduleReset(BackendLibraryPhotoFilter filter, BackendLibraryPhotoOrder order);
    [[nodiscard]] bool loadMore(bool admitted);
    [[nodiscard]] bool refreshStreamingPrefix(bool admitted);

  signals:
    void workStateChanged();
    void hasMoreChanged();
    void itemCountChanged();
    void decisionsReconciled();
    void queryStarted(const BackendLibraryPhotoFilter& filter, quint64 generation);
    void resetPresentationStarted();
    void statusMessageChanged();
    void readyStatusRequested();

  private:
    enum class PageKind : std::uint8_t {
        InitialReset,
        StreamingPrefix,
        Append,
    };

    struct PageTaskResult final {
        BackendLibraryPhotoPage page;
        QString error;
        quint64 generation = 0;
        quint64 request_id = 0;
        PageKind kind = PageKind::InitialReset;
    };

    struct CountTaskResult final {
        quint64 count = 0;
        QString error;
        quint64 generation = 0;
        quint64 request_id = 0;
    };

    [[nodiscard]] static PageTaskResult runPageTask(
        Operations operations,
        BackendLibraryPhotoFilter filter,
        BackendLibraryPhotoOrder order,
        BackendLibraryPhotoCursor cursor,
        quint64 generation,
        quint64 request_id,
        PageKind kind
    );
    [[nodiscard]] static CountTaskResult runCountTask(
        Operations operations,
        BackendLibraryPhotoFilter filter,
        quint64 generation,
        quint64 request_id
    );
    [[nodiscard]] static QVector<ReviewItem> reviewItems(QVector<BackendReviewItem> source);
    [[nodiscard]] static int boundedCount(quint64 count) noexcept;

    void beginReset();
    void startPage(PageKind kind);
    void finishPage();
    void startCount();
    void finishCount();
    void setHasMore(bool has_more);
    void publishStatus(LocalizedUiMessage status);
    void requestReadyStatus();

    Operations operations_;
    ReviewModel* model_;
    DecisionReconciler decision_reconciler_;
    BackendLibraryPhotoFilter requested_filter_;
    BackendLibraryPhotoFilter active_filter_;
    BackendLibraryPhotoOrder requested_order_ = BackendLibraryPhotoOrder::CaptureTimeDescending;
    BackendLibraryPhotoOrder active_order_ = BackendLibraryPhotoOrder::CaptureTimeDescending;
    BackendLibraryPhotoCursor next_cursor_;
    LocalizedUiMessage status_message_;
    quint64 generation_ = 1;
    quint64 page_request_id_ = 0;
    quint64 active_page_request_id_ = 0;
    quint64 count_request_id_ = 0;
    quint64 active_count_request_id_ = 0;
    quint64 total_items_ = 0;
    bool scan_running_ = false;
    bool decision_busy_ = false;
    bool page_running_ = false;
    bool page_reset_running_ = false;
    bool reset_pending_ = false;
    bool count_running_ = false;
    bool count_pending_ = false;
    bool terminal_refresh_active_ = false;
    bool has_more_ = false;
    QTimer debounce_timer_;
    QFutureWatcher<PageTaskResult> page_watcher_;
    QFutureWatcher<CountTaskResult> count_watcher_;
};
