#pragma once

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <stdexcept>

/// Owns ordered journal writes, bounded pending snapshots, sticky failure,
/// and teardown. A failed journal cannot be packaged as a complete document.
class DocumentHistoryWriter final {
  public:
    DocumentHistoryWriter() {
        pool_.setMaxThreadCount(1);
    }
    ~DocumentHistoryWriter() {
        pool_.waitForDone();
    }
    void append(std::function<void()> write) {
        if (pending_.fetch_add(1) >= 128) {
            --pending_;
            fail(
                QCoreApplication::translate(
                    "DocumentHistoryWriter",
                    "Document history could not keep up with the edits."
                )
            );
            return;
        }
        pool_.start(QRunnable::create([this, write = std::move(write)] {
            try {
                write();
            } catch (const std::exception& error) {
                fail(QString::fromUtf8(error.what()));
            }
            --pending_;
        }));
    }
    void flush() const {
        pool_.waitForDone();
        const QMutexLocker lock(&mutex_);
        if (!error_.isEmpty())
            throw std::runtime_error(error_.toStdString());
    }

  private:
    void fail(const QString& message) {
        const QMutexLocker lock(&mutex_);
        if (error_.isEmpty())
            error_ = message;
    }
    mutable QThreadPool pool_;
    mutable QMutex mutex_;
    QString error_;
    std::atomic<unsigned> pending_{0};
};
