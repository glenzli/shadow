#pragma once

#include "desktop_backend.hpp"
#include "review_model.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QUrl>

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

class ReviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY folderPathChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY itemCountChanged)
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
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;

    Q_INVOKABLE void scanFolder(const QUrl& folder_url);
    Q_INVOKABLE void loadMore();

signals:
    void busyChanged();
    void loadingMoreChanged();
    void hasMoreChanged();
    void folderPathChanged();
    void statusTextChanged();
    void itemCountChanged();

private:
    void finishScan();
    void finishPage();
    void startPage(bool reset);
    void emitWorkStateChanges(bool old_busy, bool old_loading_more);
    void setHasMore(bool has_more);
    void setStatusText(QString status);
    void updateReadyStatus();

    std::shared_ptr<DesktopBackend> backend_;
    QString folder_path_;
    QString status_text_ = QStringLiteral("Choose a folder to build your Review library");
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
    QFutureWatcher<ScanTaskResult> scan_watcher_;
    QFutureWatcher<PageTaskResult> page_watcher_;
};
