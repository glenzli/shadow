#pragma once

#include "desktop_backend.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

class EditController;

/// Asynchronous document timeline browsing and exact same-photo checkout.
class EditableDocumentHistory final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
  public:
    EditableDocumentHistory(std::shared_ptr<DesktopBackend> backend, EditController& editor);
    ~EditableDocumentHistory() override;
    QVariantList items() const {
        return items_;
    }
    bool busy() const {
        return watcher_.isRunning();
    }
    QString error() const {
        return error_;
    }
    bool hasMore() const {
        return cursor_ > 0;
    }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void restore(const QString& step);
  signals:
    void changed();

  private:
    struct Result {
        QString page;
        QString error;
        DocumentHistoryEntry step;
        DocumentUndo undo;
        bool restore = false;
        bool append = false;
        quint64 photo = 0;
        quint64 revision = 0;
    };
    void load(qint64 before, bool append);
    void finish();
    std::shared_ptr<DesktopBackend> backend_;
    EditController& editor_;
    QFutureWatcher<Result> watcher_;
    QVariantList items_;
    QString error_;
    qint64 cursor_ = 0;
};
