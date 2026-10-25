#include "storage_profile_registry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "read fixture");
    return f.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "write fixture");
}
StorageProfileLaunch launch(const QString& daily, bool debug) {
    StorageProfileLaunch value;
    value.arguments = {"Shadow"};
    value.daily_root = daily;
    value.debug_build = debug;
    return value;
}
std::unique_ptr<StorageProfile> selected(
    StorageProfileRegistry& registry,
    const StorageProfileLaunch& input,
    const QString& context
) {
    QString error;
    auto resolved = registry.resolveLaunch(input, context, &error);
    require(resolved.has_value(), "resolve launch");
    auto profile = StorageProfile::open(*resolved, &error);
    require(profile != nullptr, qPrintable(QStringLiteral("open selected launch: ") + error));
    return profile;
}
void contracts() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage");
    const QString base = QFileInfo(temporary.path()).canonicalFilePath();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, base + "/legacy");
    StorageProfileRegistry registry(base + "/daily", base + "/bootstrap");
    auto normal_input = launch(registry.dailyRoot(), false);
    auto debug_input = launch(registry.dailyRoot(), true);
    QString error;
    require(
        StorageProfileRegistry::launchContext(debug_input, &error) == "debug",
        "Debug build lane"
    );
    require(
        StorageProfileRegistry::launchContext(normal_input, &error) == "normal",
        "Release build lane"
    );
    debug_input.arguments.append(
        {"--profile-launch-context", "debug", "--profile-launch-context=normal"}
    );
    require(
        StorageProfileRegistry::launchContext(debug_input, &error).isEmpty(),
        "ambiguous context fails"
    );
    debug_input.arguments = {"Shadow"};
    error.clear();
    auto debug = selected(registry, debug_input, "debug");
    require(
        debug->kind() == "debug" && !QFileInfo::exists(registry.dailyRoot()),
        "first Debug does not enroll Daily"
    );
    require(registry.remember(debug->info(), "debug", &error), "remember Debug");
    const auto debug_info = debug->info();
    debug.reset();
    QSettings legacy;
    legacy.setValue("profile-preflight/legacy", "preserve on actual launch");
    legacy.sync();
    auto daily_preflight = registry.prepare(registry.dailyRoot(), {}, &error);
    require(
        daily_preflight && !QFileInfo::exists(registry.dailyRoot() + "/ui-preferences.ini"),
        "preflight only enrolls identity, leaving launch-owned legacy preservation pending"
    );
    auto daily = selected(registry, normal_input, "normal");
    require(daily->kind() == "daily", "first normal retains legacy daily location");
    QSettings preserved(daily->settingsFile(), QSettings::IniFormat);
    require(
        preserved.value("profile-preflight/legacy").toString() == "preserve on actual launch",
        "actual launch completes settings preservation after preflight"
    );
    const auto daily_info = daily->info();
    require(registry.remember(daily_info, "normal", &error), "normal lane remembers Daily");
    auto first = registry.create(QString::fromUtf8("旅行资料库"), base, &error);
    auto second = registry.create("Development", base, &error);
    require(
        first && second && first->root != second->root,
        "new empty libraries have distinct roots"
    );
    require(
        first->name() == QString::fromUtf8("旅行资料库")
            && !QFileInfo::exists(first->root + "/catalog.sqlite"),
        "creation preserves authored Unicode name without copying catalog"
    );
    require(registry.remember(*first, "normal", &error), "normal custom choice");
    require(registry.remember(*second, "debug", &error), "debug custom choice");
    require(
        registry.remember(daily_info, "debug", &error),
        "intentional Debug daily open records only recent use"
    );
    auto actual_debug = selected(registry, debug_input, "debug");
    auto actual_normal = selected(registry, normal_input, "normal");
    require(
        actual_debug->id() == second->id && actual_normal->id() == first->id,
        "separate launch lanes, Debug never automatically Daily"
    );
    require(
        !registry.prepare(second->root, second->id, &error),
        "active target lock blocks preflight"
    );
    error.clear();
    const QString marker = first->root + "/storage-profile.json";
    auto metadata = QJsonDocument::fromJson(read(marker)).object();
    metadata.insert("futureOwnedFlag", "preserve");
    write(marker, QJsonDocument(metadata).toJson());
    write(first->root + "/catalog.sqlite", "authored catalog bytes");
    require(actual_normal->rename("  My Daily Work  ", &error), "display name can change");
    require(
        actual_normal->root() == first->root && actual_normal->id() == first->id,
        "rename retains location and identity"
    );
    require(
        read(first->root + "/catalog.sqlite") == "authored catalog bytes",
        "rename does not alter catalog"
    );
    require(
        QJsonDocument::fromJson(read(marker)).object().value("futureOwnedFlag").toString()
            == "preserve",
        "rename preserves unrelated enrollment flags"
    );
    require(!actual_normal->rename("\n", &error), "control-only name rejected");
    error.clear();
    require(!registry.create("Nested", registry.dailyRoot(), &error), "Daily nesting rejected");
    error.clear();
    require(!registry.create("Nested", debug_info.root, &error), "Debug nesting rejected");
    error.clear();
    require(!registry.create("Nested", second->root, &error), "custom nesting rejected");
    error.clear();
    require(
        !registry.create("Nested", registry.registryRoot(), &error),
        "registry nesting rejected"
    );
    actual_debug.reset();
    actual_normal.reset();
    daily.reset();
    metadata = QJsonDocument::fromJson(read(second->root + "/storage-profile.json")).object();
    metadata.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    write(second->root + "/storage-profile.json", QJsonDocument(metadata).toJson());
    error.clear();
    require(
        !registry.resolveLaunch(debug_input, "debug", &error),
        "replaced library identity never inherits launch authority"
    );
    require(!registry.prepare(second->root, second->id, &error), "stale recent selection fails");
    error.clear();
    require(
        registry.remember(debug_info, "debug", &error),
        "explicit known Debug repairs selection"
    );
    require(
        QDir(debug_info.root).rename(debug_info.root, debug_info.root + "-moved"),
        "move fixture"
    );
    require(
        !registry.resolveLaunch(debug_input, "debug", &error)
            && !QFileInfo::exists(debug_info.root),
        "missing last profile fails without recreating or opening Daily"
    );
    const QString records = registry.registryRoot() + "/library-profiles.json";
    write(records, "invalid bootstrap metadata");
    error.clear();
    require(
        !registry.resolveLaunch(normal_input, "normal", &error),
        "corrupt registry fails closed"
    );
    auto explicit_input = normal_input;
    explicit_input.arguments.append({"--profile", "daily"});
    require(
        registry.resolveLaunch(explicit_input, "normal", &error).has_value(),
        "explicit recovery is independent of corrupt remembered selection"
    );
    error.clear();
    require(registry.resetRecentRecords(&error), "deliberate record reset");
    const auto backups =
        QDir(registry.registryRoot()).entryList({"library-profiles.json.preserved-*"}, QDir::Files);
    require(
        backups.size() == 1
            && read(registry.registryRoot() + '/' + backups.front())
                   == "invalid bootstrap metadata",
        "reset preserves prior records"
    );
    require(
        read(first->root + "/catalog.sqlite") == "authored catalog bytes",
        "record reset never touches library content"
    );
    for (int i = 0; i < 15; ++i) {
        auto created = registry.create(QString::number(i), base, &error);
        require(created && registry.remember(*created, {}, &error), "register fresh recent entry");
    }
    require(registry.recent(&error).size() == 14, "bounded recent list plus built-in choices");
    auto final_debug = selected(registry, debug_input, "debug");
    require(final_debug->kind() == "debug", "reset Debug defaults to independent test library");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ShadowRegistryContract");
    QCoreApplication::setApplicationName("Registry");
    try {
        contracts();
        std::cout << "profile registry contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
