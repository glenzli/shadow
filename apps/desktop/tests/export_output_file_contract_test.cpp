#include "backend/export_output_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create competing file");
    require(file.write(bytes) == bytes.size(), "write competing bytes");
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read published file");
    return file.readAll();
}

void successfulPublication() {
    QTemporaryDir root;
    require(root.isValid(), "temporary root");
    const auto path = root.filePath(QStringLiteral("照片.png"));
    {
        ExportOutputFile output(path);
        require(output.device().write("complete raster") == 15, "write staging bytes");
        require(!QFileInfo::exists(path), "destination stays absent while staging");
        require(output.commit() == 15, "receipt length matches output");
    }
    require(read(path) == "complete raster", "published output survives writer destruction");
    require(
        QDir(root.path()).entryList(QDir::Files | QDir::Hidden).size() == 1,
        "successful publication leaves no staging file"
    );
}

void lateCollision() {
    QTemporaryDir root;
    const auto path = root.filePath(QStringLiteral("output.png"));
    bool conflict = false;
    {
        ExportOutputFile output(path);
        require(output.device().write("our raster") == 10, "write staged raster");
        write(path, "other owner");
        try {
            static_cast<void>(output.commit());
        } catch (const ExportOutputConflict&) {
            conflict = true;
        }
    }
    require(conflict, "a file created during encoding must report a conflict");
    require(read(path) == "other owner", "late collision preserves the other owner's bytes");
    require(
        QDir(root.path()).entryList(QDir::Files | QDir::Hidden).size() == 1,
        "conflict removes staging file"
    );
}

void competingWriters() {
    QTemporaryDir root;
    const auto path = root.filePath(QStringLiteral("output.png"));
    bool conflict = false;
    {
        ExportOutputFile first(path);
        ExportOutputFile second(path);
        require(first.device().write("first") == 5, "first writer");
        require(second.device().write("second") == 6, "second writer");
        require(first.commit() == 5, "first publication wins");
        try {
            static_cast<void>(second.commit());
        } catch (const ExportOutputConflict&) {
            conflict = true;
        }
    }
    require(conflict && read(path) == "first", "two staged writers cannot replace each other");
}

void directoryCollision() {
    QTemporaryDir root;
    const auto path = root.filePath(QStringLiteral("output.png"));
    bool conflict = false;
    {
        ExportOutputFile output(path);
        require(output.device().write("raster") == 6, "stage before directory collision");
        require(QDir().mkdir(path), "create competing directory");
        try {
            static_cast<void>(output.commit());
        } catch (const ExportOutputConflict&) {
            conflict = true;
        }
    }
    require(conflict && QFileInfo(path).isDir(), "late directory collision remains untouched");
}

void abandonedWrite() {
    QTemporaryDir root;
    const auto path = root.filePath(QStringLiteral("output.png"));
    try {
        ExportOutputFile output(path);
        require(output.device().write("partial") == 7, "write incomplete raster");
        throw std::runtime_error("synthetic encoder failure");
    } catch (const std::runtime_error&) {}
    require(!QFileInfo::exists(path), "failed encoder cannot publish a partial output");
    require(
        QDir(root.path()).entryList(QDir::Files | QDir::Hidden).isEmpty(),
        "abandoned output removes staging bytes"
    );
}

void invalidDestinations() {
    QTemporaryDir root;
    bool empty_rejected = false;
    bool missing_parent_rejected = false;
    try {
        ExportOutputFile output(QString{});
    } catch (const std::invalid_argument&) {
        empty_rejected = true;
    }
    try {
        ExportOutputFile output(root.filePath(QStringLiteral("missing/output.png")));
    } catch (const std::runtime_error&) {
        missing_parent_rejected = true;
    }
    require(empty_rejected && missing_parent_rejected, "invalid destinations are rejected");
    const auto path = root.filePath(QStringLiteral("occupied.png"));
    write(path, "existing");
    bool conflict = false;
    try {
        ExportOutputFile output(path);
    } catch (const ExportOutputConflict&) {
        conflict = true;
    }
    require(conflict && read(path) == "existing", "existing file remains unchanged");
}

#ifdef Q_OS_UNIX
void danglingSymlink(const bool late) {
    QTemporaryDir root;
    const auto path = root.filePath(QStringLiteral("output.png"));
    const auto target = root.filePath(QStringLiteral("absent.png"));
    if (!late) {
        require(QFile::link(target, path), "create dangling symlink");
    }
    bool conflict = false;
    try {
        ExportOutputFile output(path);
        require(output.device().write("raster") == 6, "write behind dangling link");
        if (late) {
            require(QFile::link(target, path), "create late dangling symlink");
        }
        static_cast<void>(output.commit());
    } catch (const ExportOutputConflict&) {
        conflict = true;
    }
    require(conflict && QFileInfo(path).isSymLink(), "dangling symlink is an occupied destination");
    require(!QFileInfo::exists(target), "export never creates a symlink target");
}
#endif

} // namespace

int main() {
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    run("complete publication", successfulPublication);
    run("late collision", lateCollision);
    run("competing writers", competingWriters);
    run("late directory collision", directoryCollision);
    run("encoder failure cleanup", abandonedWrite);
    run("invalid destinations", invalidDestinations);
#ifdef Q_OS_UNIX
    run("dangling symlink", [] { danglingSymlink(false); });
    run("late dangling symlink", [] { danglingSymlink(true); });
#endif
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
