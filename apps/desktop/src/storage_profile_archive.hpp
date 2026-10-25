#pragma once

#include <QObject>
#include <QString>

#include <memory>

struct StorageProfileArchiveRequest final {
    // 0 backup; 1 restore into a new library; 2 test copy; 3 verify backup.
    quint8 action = 0;
    QString source;
    QString parent;
    QString name;
    QString expected_id;
};

// Worker lifetime, cancellation and native-path transport for verified profile
// snapshots. Shared application admission and draft saving stay with management.
class StorageProfileArchive final : public QObject {
    Q_OBJECT
  public:
    explicit StorageProfileArchive(QObject* parent = nullptr);
    ~StorageProfileArchive() override;
    void start(const StorageProfileArchiveRequest& request);
    void cancel();
  signals:
    void completed(const QString& root);
    void failed(const QString& diagnostic);
    void progress(quint64 completed_files, quint64 total_files);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
