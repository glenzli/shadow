#include "backend/export_output_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <string>
#include <system_error>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif
#ifdef Q_OS_DARWIN
#include <stdio.h>
#endif

namespace {

bool occupied(const QString& path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

void syncFile(QTemporaryFile& file) {
    if (file.error() != QFileDevice::NoError || !file.flush()
        || file.error() != QFileDevice::NoError) {
        throw std::runtime_error(
            std::string("could not publish export atomically: ") + file.errorString().toStdString()
        );
    }
    int result = 0;
    do {
#ifdef Q_OS_WIN
        result = ::_commit(file.handle());
#else
        result = ::fsync(file.handle());
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "could not synchronize export output"
        );
    }
}

} // namespace

ExportOutputConflict::ExportOutputConflict(const QString& destination_path) :
    std::runtime_error(
        std::string("export destination already exists: ") + destination_path.toStdString()
    ) {}

ExportOutputFile::ExportOutputFile(const QString& destination_path) :
    destination_path_(QFileInfo(destination_path).absoluteFilePath()),
    file_(
        QFileInfo(destination_path_).absoluteDir().filePath(QStringLiteral(".shadow-export-XXXXXX"))
    ) {
    if (destination_path.isEmpty()) {
        throw std::invalid_argument("export destination path is empty");
    }
    if (occupied(destination_path_)) {
        throw ExportOutputConflict(destination_path_);
    }
    if (!file_.open()) {
        throw std::runtime_error(
            std::string("could not open export destination: ") + file_.errorString().toStdString()
        );
    }
}

qint64 ExportOutputFile::StagingFile::writeData(const char* data, const qint64 length) {
    const qint64 written = QTemporaryFile::writeData(data, length);
    // Like QSaveFile, remember a failed write even if a later successful
    // encoder operation clears QFileDevice's current error.
    write_failed_ = write_failed_ || written != length;
    return written;
}

QIODevice& ExportOutputFile::device() noexcept {
    return file_;
}

std::uint64_t ExportOutputFile::commit() {
    if (!file_.isOpen()) {
        throw std::logic_error("export output is no longer open for publication");
    }
    if (file_.writeFailed() || file_.error() != QFileDevice::NoError) {
        throw std::runtime_error("could not publish export after a failed write");
    }
    // Materialize an unnamed temporary file before publishing by native path.
#ifndef Q_OS_WIN
    const QString staged_path = file_.fileName();
#endif
    const auto byte_length = static_cast<std::uint64_t>(file_.size());
    syncFile(file_);
    file_.close();
#ifdef Q_OS_WIN
    // QTemporaryFile closes its native handle before MoveFileW, without
    // replacement or QFile's copy/delete fallback.
    if (!file_.rename(destination_path_)) {
        if (occupied(destination_path_)) {
            throw ExportOutputConflict(destination_path_);
        }
        throw std::runtime_error(
            std::string("could not publish export atomically: ") + file_.errorString().toStdString()
        );
    }
    file_.setAutoRemove(false);
#else
    const auto source = QFile::encodeName(staged_path);
    const auto destination = QFile::encodeName(destination_path_);
#ifdef Q_OS_DARWIN
    // Do not use Qt's Unix rename fallback: some unsupported filesystems can
    // fall back to replacing rename(). Exclusivity is the publication contract.
    const int result = ::renamex_np(source.constData(), destination.constData(), RENAME_EXCL);
#else
    // A hard link publishes the complete file atomically without replacement.
    // The temporary owner removes the staging name after publication.
    const int result = ::link(source.constData(), destination.constData());
#endif
    if (result != 0) {
        const int error = errno;
        if (error == EEXIST || error == ENOTEMPTY) {
            throw ExportOutputConflict(destination_path_);
        }
        throw std::system_error(
            error,
            std::generic_category(),
            "could not publish export atomically"
        );
    }
#ifdef Q_OS_DARWIN
    file_.setAutoRemove(false);
#else
    static_cast<void>(file_.remove());
#endif
#endif
    return byte_length;
}
