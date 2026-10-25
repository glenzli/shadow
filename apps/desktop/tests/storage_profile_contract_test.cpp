#include "storage_profile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#if defined(Q_OS_WIN)
#include <QProcess>
#include <qt_windows.h>
#endif

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void write(const QString& path, const QByteArray& bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture parent");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}

bool directoryAlias(const QString& target, const QString& alias) {
#if defined(Q_OS_WIN)
    // QFile::link creates a shell shortcut on Windows. A junction exercises
    // the actual filesystem boundary without requiring symbolic-link rights.
    QProcess process;
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    process.start(
        QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/cmd.exe"),
        {"/d",
         "/c",
         "mklink",
         "/J",
         QDir::toNativeSeparators(alias),
         QDir::toNativeSeparators(target)}
    );
    return process.waitForFinished(10000) && process.exitCode() == 0
           && QFileInfo(alias).isJunction();
#else
    return QFile::link(target, alias);
#endif
}

void profileContracts() {
    QTemporaryDir fixture;
    require(fixture.isValid(), "create isolated fixture");
    const QString fixture_root = QFileInfo(fixture.path()).canonicalFilePath();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture_root + "/legacy-config");
    QCoreApplication::setOrganizationName("ShadowProfileContract");
    QCoreApplication::setApplicationName("desktop");
    QSettings legacy;
    legacy.setValue("appearance/mode", "dark");
    legacy.setValue("smartCategories/custom", "daily category");
    legacy.setValue("export/output", "/daily/export");
    legacy.sync();
    const QByteArray legacy_bytes = read(legacy.fileName());
    const QString daily_root = fixture_root + "/existing-daily";
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    require(
        StorageProfile::rootsOverlap(daily_root, fixture_root + "/EXISTING-DAILY"),
        "reserved Daily location also excludes case aliases before enrollment"
    );
    require(
        StorageProfile::rootsOverlap(
            daily_root + "-debug",
            fixture_root + "/EXISTING-DAILY-DEBUG/nested"
        ),
        "Debug location excludes nested case aliases"
    );
#endif
#if defined(Q_OS_WIN)
    require(
        StorageProfile::rootsOverlap(QStringLiteral("C:/"), QStringLiteral("C:/Users/profile")),
        "drive root cannot enclose a separate library"
    );
#else
    require(
        StorageProfile::rootsOverlap(QStringLiteral("/"), daily_root),
        "filesystem root cannot enclose a separate library"
    );
#endif
    const QString brush_source = fixture_root + "/legacy-brushes.ini";
    write(brush_source, "[General]\nversion=1\nactive=1\n");
    const QStringList preserved{
        "catalog.sqlite",
        "catalog.sqlite-wal",
        "catalog.sqlite-shm",
        "derived-rasters/authored-mask",
        "people/authored-person",
        "cache/original-preview",
        "remote-library/checkpoint",
        "lut-store/custom.cube"
    };
    for (const QString& path : preserved)
        write(QDir(daily_root).filePath(path), path.toUtf8());
    {
        QSettings current(daily_root + "/ui-preferences.ini", QSettings::IniFormat);
        current.setValue("appearance/mode", "light");
        current.sync();
    }

    StorageProfileLaunch launch;
    launch.arguments = {"Shadow"};
    launch.daily_root = daily_root;
    launch.legacy_brush_file = brush_source;
    launch.debug_build = true;
    QString error;
    auto debug = StorageProfile::open(launch, &error);
    require(debug != nullptr, "Debug default opens an independent profile");
    require(
        debug->kind() == "debug" && debug->root() == daily_root + "-debug",
        "Debug default root"
    );
    require(
        !QFileInfo::exists(daily_root + "/storage-profile.json"),
        "Debug does not enroll Daily"
    );
    require(!QFileInfo::exists(debug->settingsFile()), "Debug never copies global preferences");
    require(
        !QFileInfo::exists(debug->brushSettingsFile()),
        "Debug never copies Daily brush presets"
    );
    const QString debug_id = debug->id();
    StorageProfileLaunch server_launch = launch;
    server_launch.process = StorageProfileProcess::LibraryServer;
    auto server = StorageProfile::open(server_launch, &error);
    require(
        server && server->id() == debug_id && server->root() == debug->root(),
        "independent Server joins the selected profile while Desktop is open"
    );
    require(
        server->settingsFile() != debug->settingsFile(),
        "Server settings have their own owner"
    );
    require(
        !QFileInfo::exists(server->settingsFile()),
        "Debug Server never imports global settings"
    );
    require(
        !StorageProfile::open(server_launch, &error),
        "one independent Server writer per profile"
    );
    server.reset();
    auto competing_debug = StorageProfile::open(launch, &error);
    require(!competing_debug && error.contains("already open"), "one writer per profile");

    launch.debug_build = false;
    auto daily = StorageProfile::open(launch, &error);
    require(
        daily != nullptr && daily->isDaily() && daily->root() == daily_root,
        qPrintable(QStringLiteral("Release preserves Daily root: ") + error)
    );
    QSettings migrated(daily->settingsFile(), QSettings::IniFormat);
    require(migrated.value("appearance/mode").toString() == "light", "local values win migration");
    require(
        migrated.value("smartCategories/custom").toString() == "daily category",
        "global categories preserved"
    );
    require(
        migrated.value("export/output").toString() == "/daily/export",
        "global export settings preserved"
    );
    require(
        read(daily->brushSettingsFile()) == read(brush_source),
        "brush presets copied without moving legacy file"
    );
    const QString daily_id = daily->id();
    require(daily_id != debug_id, "profiles have stable independent identities");
    for (const QString& path : preserved)
        require(
            read(QDir(daily_root).filePath(path)) == path.toUtf8(),
            "existing user state remains byte-identical"
        );
    require(
        read(legacy.fileName()) == legacy_bytes,
        "legacy global preferences remain byte-identical"
    );
    require(debug->bindProcessEnvironment(), "bind profile for implicit settings consumers");
    require(
        qEnvironmentVariable("SHADOW_DESKTOP_DATA_ROOT") == debug->root(),
        "resolved root reaches implicit consumers"
    );
    require(
        qEnvironmentVariable("SHADOW_PAINT_BRUSH_SETTINGS_FILE") == debug->brushSettingsFile(),
        "brush writes are scoped"
    );

    write(debug->catalogPath(), "debug edits");
    write(debug->root() + "/derived-rasters/authored-mask", "debug mask");
    {
        QSettings debug_settings(debug->settingsFile(), QSettings::IniFormat);
        debug_settings.setValue("smartCategories/custom", "debug category");
        debug_settings.sync();
    }
    for (const QString& path : preserved)
        require(
            read(QDir(daily_root).filePath(path)) == path.toUtf8(),
            "Debug writes do not change Daily"
        );
    require(
        migrated.value("smartCategories/custom").toString() == "daily category",
        "settings stay separate"
    );
    daily.reset();
    debug.reset();
    legacy.setValue("late/global/value", "must not be imported again");
    legacy.sync();
    daily = StorageProfile::open(launch, &error);
    require(daily && daily->id() == daily_id, "Daily enrollment is stable across reopen");
    QSettings reopened(daily->settingsFile(), QSettings::IniFormat);
    require(!reopened.contains("late/global/value"), "migration runs once");
    launch.debug_build = true;
    debug = StorageProfile::open(launch, &error);
    require(
        debug && debug->id() == debug_id && read(debug->catalogPath()) == "debug edits",
        "Debug reopen retains state"
    );

    const auto rejects = [&](const QStringList& arguments) {
        StorageProfileLaunch invalid = launch;
        invalid.arguments = arguments;
        return !StorageProfile::open(invalid, &error);
    };
    require(rejects({"Shadow", "--profile", "unknown"}), "invalid selection fails closed");
    require(
        rejects({"Shadow", "--profile", "debug", "--profile=daily"}),
        "duplicate options fail closed"
    );
    require(rejects({"Shadow", "--profile-root"}), "missing root fails closed");
    require(rejects({"Shadow", "--profile-root=relative"}), "relative root fails closed");
    require(rejects({"Shadow", "--profile-root=" + daily_root}), "custom cannot alias Daily");
    require(
        rejects({"Shadow", "--profile-root=" + daily_root + "/nested"}),
        "custom cannot nest inside Daily"
    );
    require(rejects({"Shadow", "--profile-root=" + fixture_root}), "custom cannot contain Daily");
    const QString alias = fixture_root + "/daily-alias";
    require(directoryAlias(daily_root, alias), "create directory alias");
    require(
        rejects({"Shadow", "--profile-root=" + alias}),
        "symlink alias cannot bypass separation"
    );
    const QString escaped = fixture_root + "/escaped";
    require(QDir().mkpath(escaped), "create escaped profile");
    require(directoryAlias(daily_root + "/cache", escaped + "/cache"), "create cache alias");
    require(
        rejects({"Shadow", "--profile-root=" + escaped}),
        "mutable store cannot escape its profile"
    );
    const QString blocked = fixture_root + "/not-a-directory";
    write(blocked, "file");
    require(
        rejects({"Shadow", "--profile-root=" + blocked}),
        "unavailable custom root has no fallback"
    );
    StorageProfileLaunch empty_override = launch;
    empty_override.override_root_set = true;
    require(!StorageProfile::open(empty_override, &error), "empty legacy override has no fallback");
    StorageProfileLaunch custom_launch = launch;
    custom_launch.override_root_set = true;
    custom_launch.override_root = fixture_root + "/explicit";
    auto custom = StorageProfile::open(custom_launch, &error);
    require(custom && custom->kind() == "custom", "legacy explicit root remains supported");
    custom_launch.arguments = {"Shadow", "--profile", "daily"};
    require(
        !StorageProfile::open(custom_launch, &error),
        "ambiguous named and explicit roots fail closed"
    );
    daily.reset();
    launch.arguments = {"Shadow", "--profile", "daily"};
    daily = StorageProfile::open(launch, &error);
    require(
        daily && daily->id() == daily_id,
        "explicit Daily opens existing data in a Debug build"
    );
    debug.reset();
    write(daily_root + "-debug/storage-profile.json", "{\"schemaVersion\":99}");
    launch.arguments = {"Shadow"};
    require(
        !StorageProfile::open(launch, &error),
        "incompatible metadata is never reset or redirected"
    );
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        profileContracts();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
