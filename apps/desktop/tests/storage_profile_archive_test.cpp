#include "backend/native_path_input.hpp"
#include "storage_profile.hpp"
#include "storage_profile_archive.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(
        file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
        "write archive fixture"
    );
}
void contracts() {
    QTemporaryDir fixture;
    const auto base = QFileInfo(fixture.path()).canonicalFilePath();
    StorageProfileLaunch launch;
    launch.daily_root = base + "/daily";
    launch.arguments = {"Shadow", "--profile-root", base + "/source"};
    QString error;
    auto profile = StorageProfile::open(launch, &error);
    require(profile != nullptr, "own source library");
    const auto session = shadow::desktop::open_desktop_session(
        native_path_input::path(profile->catalogPath()),
        native_path_input::path(profile->cacheRoot())
    );
    write(profile->root() + "/derived-rasters/authored.bin", "synthetic attachment");
    QSettings settings(profile->settingsFile(), QSettings::IniFormat);
    settings.setValue("ai/image_understanding_background_enabled", true);
    settings.setValue("ai/image_understanding_execution_allowed", true);
    settings.setValue("library-server/auto_start", true);
    settings.setValue("ui/language", "zh_CN");
    settings.sync();
    StorageProfileArchive worker;
    QString result, failure;
    bool finished = false;
    QObject::connect(&worker, &StorageProfileArchive::completed, [&](const QString& value) {
        result = value;
        finished = true;
    });
    QObject::connect(&worker, &StorageProfileArchive::failed, [&](const QString& value) {
        failure = value;
        finished = true;
    });
    const auto run = [&](StorageProfileArchiveRequest request) {
        result.clear();
        failure.clear();
        finished = false;
        worker.start(request);
        require(
            QTest::qWaitFor([&] { return finished; }, 10000),
            "archive worker completes without blocking UI"
        );
    };
    run({0, profile->root(), base, {}, profile->id()});
    require(
        failure.isEmpty() && QFileInfo::exists(result + "/shadow-library-backup.json"),
        "actual linked backup publishes its verified manifest"
    );
    const auto backup = result;
    run({1, backup, base, "Restored", {}});
    require(failure.isEmpty(), qPrintable(QStringLiteral("linked restore succeeds: ") + failure));
    const auto restored = StorageProfile::inspect(result, launch.daily_root, &error);
    require(
        restored && restored->id != profile->id() && restored->name() == "Restored",
        "restored profile has independent identity"
    );
    QSettings restoredSettings(result + "/ui-preferences.ini", QSettings::IniFormat);
    require(
        !restoredSettings.value("ai/image_understanding_background_enabled", true).toBool()
            && !restoredSettings.value("ai/image_understanding_execution_allowed", true).toBool()
            && !restoredSettings.value("library-server/auto_start", true).toBool(),
        "Qt reads copied preferences with automatic admission disabled"
    );
    require(
        restoredSettings.value("ui/language").toString() == "zh_CN",
        "other copied preferences survive"
    );
    QLockFile server(profile->root() + "/server-manager.lock");
    server.setStaleLockTime(0);
    require(server.tryLock(), "fixture server writer");
    run({0, profile->root(), base, {}, profile->id()});
    require(
        result.isEmpty() && !failure.isEmpty(),
        "active independent server prevents a consistency cut"
    );
    server.unlock();
    write(backup + "/payload/derived-rasters/authored.bin", "corrupted");
    run({3, backup, {}, {}, {}});
    require(
        result.isEmpty() && !failure.isEmpty(),
        "linked verification detects attachment corruption"
    );
    require(
        QFileInfo::exists(profile->root() + "/storage-profile.lock"),
        "source writer remains owned through every operation"
    );
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        contracts();
        std::cout << "profile archive worker contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
