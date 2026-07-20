#pragma once

#include "review_model.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QUrl>

struct ScanTaskResult final {
    QVector<ReviewItem> items;
    QString folder_path;
    QString error;
    quint64 files_seen = 0;
    quint64 supported_files = 0;
    quint64 decode_queued = 0;
    quint64 issue_count = 0;
};

class ReviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY folderPathChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY itemCountChanged)
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)

public:
    ReviewController(QString catalog_path, QString cache_root, QObject* parent = nullptr);
    ~ReviewController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString folderPath() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] int itemCount() const;
    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] ReviewModel* reviewModel() noexcept;

    Q_INVOKABLE void scanFolder(const QUrl& folder_url);

signals:
    void busyChanged();
    void folderPathChanged();
    void statusTextChanged();
    void itemCountChanged();

private:
    void finishScan();
    void setBusy(bool busy);
    void setStatusText(QString status);

    QString catalog_path_;
    QString cache_root_;
    QString folder_path_;
    QString status_text_ = QStringLiteral("Choose a folder to build your Review library");
    bool busy_ = false;
    ReviewModel model_;
    QFutureWatcher<ScanTaskResult> watcher_;
};
