#pragma once

#include "desktop_backend.hpp"
#include "review_evidence_session.hpp"
#include "review_model.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <cstdint>
#include <memory>

struct ScanTaskResult final {
    BackendScanReport report;
    QString error;
    quint64 generation = 0;
};

struct PageTaskResult final {
    BackendReviewPage page;
    QString error;
    quint64 generation = 0;
    bool reset = false;
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

class ReviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY folderPathChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
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
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)

public:
    explicit ReviewController(
        std::shared_ptr<DesktopBackend> backend,
        QObject* parent = nullptr
    );
    ~ReviewController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool loadingMore() const noexcept;
    [[nodiscard]] bool hasMore() const noexcept;
    [[nodiscard]] QString folderPath() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] int itemCount() const;
    [[nodiscard]] bool comparisonBusy() const noexcept;
    [[nodiscard]] bool canUndoComparison() const noexcept;
    [[nodiscard]] int sessionEvidenceCount() const noexcept;
    [[nodiscard]] QString comparisonStatusText() const;
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;

    Q_INVOKABLE void scanFolder(const QUrl& folder_url);
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

signals:
    void busyChanged();
    void loadingMoreChanged();
    void hasMoreChanged();
    void folderPathChanged();
    void statusTextChanged();
    void itemCountChanged();
    void comparisonStateChanged();
    void comparisonStatusTextChanged();
    void comparisonRecorded();
    void comparisonForgotten();

private:
    void finishScan();
    void finishPage();
    void finishEvidenceTask();
    void startPage(bool reset);
    void emitWorkStateChanges(bool old_busy, bool old_loading_more);
    void setHasMore(bool has_more);
    void setStatusText(QString status);
    void updateReadyStatus();
    void setComparisonStatusText(QString status);

    std::shared_ptr<DesktopBackend> backend_;
    QString folder_path_;
    QString status_text_ = QStringLiteral("Choose a folder to build your Review library");
    QString comparison_status_text_ = QStringLiteral(
        "Explicit choices are recorded as evidence; no preference model is active"
    );
    QString next_cursor_path_;
    QString next_cursor_representation_id_;
    quint64 generation_ = 0;
    quint64 total_items_ = 0;
    quint64 supported_files_ = 0;
    quint64 decode_queued_ = 0;
    quint64 issue_count_ = 0;
    bool scan_running_ = false;
    bool page_running_ = false;
    bool has_more_ = false;
    ReviewModel model_;
    ReviewEvidenceSession evidence_session_;
    QFutureWatcher<ScanTaskResult> scan_watcher_;
    QFutureWatcher<PageTaskResult> page_watcher_;
    QFutureWatcher<ReviewEvidenceTaskResult> evidence_watcher_;
};
