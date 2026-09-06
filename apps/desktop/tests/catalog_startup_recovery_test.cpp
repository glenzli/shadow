#include "catalog_startup_recovery.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir fixture;
    const QDir root(fixture.path());
    for (const auto& name :
         {"catalog.sqlite",
          "catalog.sqlite-wal",
          "catalog.sqlite-shm",
          "people/groups.json",
          "cache/local/data",
          "cache/remote-library-previews/data"}) {
        const QString path = root.filePath(QString::fromLatin1(name));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(name) < 0)
            return EXIT_FAILURE;
    }
    QString error;
    if (!reset_local_development_catalog(
            root.filePath("catalog.sqlite"),
            root.filePath("cache"),
            &error
        )) {
        std::cerr << error.toStdString();
        return EXIT_FAILURE;
    }
    const QDir recovery(root.filePath("catalog-recovery"));
    const auto dirs = recovery.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (dirs.size() != 1 || QFileInfo::exists(root.filePath("catalog.sqlite")))
        return EXIT_FAILURE;
    const QDir saved(recovery.filePath(dirs.front()));
    for (const auto& name :
         {"catalog.sqlite",
          "catalog.sqlite-wal",
          "catalog.sqlite-shm",
          "people/groups.json",
          "cache/local/data"}) {
        QFile file(saved.filePath(QString::fromLatin1(name)));
        if (!file.open(QIODevice::ReadOnly) || file.readAll() != QByteArray(name))
            return EXIT_FAILURE;
    }
    if (!QFileInfo::exists(root.filePath("cache/remote-library-previews/data")))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
