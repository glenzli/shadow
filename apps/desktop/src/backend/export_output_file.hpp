#pragma once

#include <QString>
#include <QTemporaryFile>

#include <cstdint>
#include <stdexcept>

class ExportOutputConflict final : public std::runtime_error {
  public:
    explicit ExportOutputConflict(const QString& destination_path);
};

/// Stages one complete encoded raster alongside its destination. An abandoned
/// writer removes its temporary bytes; commit synchronizes the file and publishes
/// without replacing any destination entry, including a dangling symbolic link.
/// Filesystems that cannot publish exclusively fail without an overwrite fallback.
class ExportOutputFile final {
  public:
    explicit ExportOutputFile(const QString& destination_path);
    /// Borrowed only for encoding before commit; do not retain or reopen it.
    [[nodiscard]] QIODevice& device() noexcept;
    [[nodiscard]] std::uint64_t commit();

  private:
    class StagingFile final : public QTemporaryFile {
      public:
        using QTemporaryFile::QTemporaryFile;
        [[nodiscard]] bool writeFailed() const noexcept {
            return write_failed_;
        }

      protected:
        qint64 writeData(const char* data, qint64 length) override;

      private:
        bool write_failed_ = false;
    };

    QString destination_path_;
    StagingFile file_;
};
