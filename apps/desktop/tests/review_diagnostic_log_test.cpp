#include "review_diagnostic_log.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Review diagnostic log contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "the diagnostic file is readable");
    return file.readAll();
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "the fixture file opens");
    require(file.write(bytes) == bytes.size(), "the fixture file is complete");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--crash-child"))) {
        ReviewDiagnosticLog log;
        require(log.configure(application.arguments().at(2)), "child log configured");
        require(
            log.append(QStringLiteral("Review diagnostics page_errors=1")),
            "child failure summary persisted"
        );
        std::_Exit(17);
    }

    QTemporaryDir temporary;
    require(temporary.isValid(), "private diagnostic directory exists");
    const QString log_dir = QDir(temporary.path()).filePath(QStringLiteral("diagnostics"));
    require(QDir().mkpath(log_dir), "diagnostic directory can be prepared");
    const QString current = QDir(log_dir).filePath(QStringLiteral("review.log"));
    const QString previous = QDir(log_dir).filePath(QStringLiteral("review.previous.log"));
    writeFile(current, QByteArray(ReviewDiagnosticLog::max_file_bytes, 'x'));

    ReviewDiagnosticLog log;
    require(log.configure(temporary.path()), "existing full log rotates on session start");
    require(log.configured(), "configured sink reports its state");
    require(
        QFileInfo(previous).size() == ReviewDiagnosticLog::max_file_bytes,
        "the prior log is preserved within its cap"
    );
    require(
        readFile(current).contains("Review diagnostics session_start"),
        "the new run has a timestamped session marker"
    );
    require(
        log.append(QStringLiteral("Review diagnostics page_backend=1,p95_le_ms:32")),
        "a bounded summary is flushed"
    );

    QProcess child;
    child.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--crash-child"), temporary.path()}
    );
    require(child.waitForFinished(10'000), "abnormal-exit child finishes");
    require(child.exitCode() == 17, "child exits without normal destruction");
    require(
        readFile(current).contains("page_errors=1"),
        "the completed failure summary survives abnormal process exit"
    );

    writeFile(current, QByteArray(ReviewDiagnosticLog::max_file_bytes, 'y'));
    require(
        log.append(QStringLiteral("Review diagnostics after_rotation=1")),
        "a second rotation replaces the old archive"
    );
    require(
        QFileInfo(current).size() <= ReviewDiagnosticLog::max_file_bytes
            && QFileInfo(previous).size() <= ReviewDiagnosticLog::max_file_bytes,
        "both retained files remain within the fixed cap"
    );
    require(
        readFile(current).contains("after_rotation=1"),
        "rotation retains the newest completed summary"
    );
    return EXIT_SUCCESS;
}
