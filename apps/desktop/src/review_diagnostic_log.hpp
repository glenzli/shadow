#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QString>

// A private, two-file journal for already-summarized Review diagnostics.
// Each append is flushed before returning, so a process crash retains the
// last completed summary. This is not a general Qt message handler.
class ReviewDiagnosticLog final {
  public:
    static constexpr qint64 max_file_bytes = 256 * 1024;

    [[nodiscard]] bool configure(const QString& application_data) {
        if (application_data.isEmpty()) {
            return false;
        }
        const QString directory = QDir(application_data).filePath(QStringLiteral("diagnostics"));
        if (!QDir().mkpath(directory) || QFileInfo(directory).isSymbolicLink()) {
            return false;
        }
        current_path_ = QDir(directory).filePath(QStringLiteral("review.log"));
        previous_path_ = QDir(directory).filePath(QStringLiteral("review.previous.log"));
        lock_path_ = QDir(directory).filePath(QStringLiteral("review.lock"));
        if (QFileInfo(current_path_).isSymbolicLink() || QFileInfo(previous_path_).isSymbolicLink()
            || QFileInfo(lock_path_).isSymbolicLink()) {
            return false;
        }
        if (!append(QStringLiteral("Review diagnostics session_start"))) {
            return false;
        }
        configured_ = true;
        return true;
    }

    [[nodiscard]] bool configured() const noexcept {
        return configured_;
    }

    [[nodiscard]] bool append(const QString& summary) {
        if (current_path_.isEmpty() || QFileInfo(current_path_).isSymbolicLink()
            || QFileInfo(previous_path_).isSymbolicLink()
            || QFileInfo(lock_path_).isSymbolicLink()) {
            return false;
        }
        QLockFile lock(lock_path_);
        if (!lock.tryLock(50)) {
            return false;
        }
        // The source is a fixed-field summary. Keep a hard line cap as a
        // second bound if future fields are added.
        const QByteArray line = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                                 + QLatin1Char(' ') + summary.left(2048) + QLatin1Char('\n'))
                                    .toUtf8();
        QFile current(current_path_);
        if (!current.open(QIODevice::WriteOnly | QIODevice::Append)) {
            return false;
        }
        if (!pruneOversizedPrevious()) {
            return false;
        }
        if (current.size() + line.size() > max_file_bytes) {
            const bool discard_oversized = current.size() > max_file_bytes;
            current.close();
            if (discard_oversized) {
                if (!QFile::remove(current_path_)) {
                    return false;
                }
            } else {
                if (QFile::exists(previous_path_) && !QFile::remove(previous_path_)) {
                    return false;
                }
                if (!QFile::rename(current_path_, previous_path_)) {
                    return false;
                }
            }
            current.setFileName(current_path_);
            if (!current.open(QIODevice::WriteOnly | QIODevice::Append)) {
                return false;
            }
        }
        const bool written = current.write(line) == line.size() && current.flush();
        current.close();
        if (written) {
            (void)QFile::setPermissions(
                current_path_,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner
            );
        }
        return written;
    }

  private:
    [[nodiscard]] bool pruneOversizedPrevious() const {
        const QFileInfo previous(previous_path_);
        return !previous.exists() || previous.size() <= max_file_bytes
               || QFile::remove(previous_path_);
    }

    QString current_path_;
    QString previous_path_;
    QString lock_path_;
    bool configured_ = false;
};
