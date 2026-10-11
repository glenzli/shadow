#include "storage_profile_registry.hpp"
#include "storage_profile_switch.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
int child(QCoreApplication& app) {
    StorageProfileSwitchEndpoint endpoint(app.arguments());
    require(endpoint.valid() && endpoint.requested(), "child restart endpoint");
    const QString root = app.arguments()[app.arguments().indexOf("--profile-root") + 1];
    StorageProfileLaunch launch;
    launch.daily_root = QFileInfo(root).absolutePath() + "/daily";
    launch.arguments = app.arguments();
    QString error;
    auto profile = StorageProfile::open(launch, &error);
    if (!profile) {
        endpoint.reportFailure(error);
        return EXIT_FAILURE;
    }
    // Startup must have removed the parent's bound storage/brush and pending
    // import/smoke environment; otherwise a real child would enter wrong owners.
    require(
        !qEnvironmentVariableIsSet("SHADOW_DESKTOP_DATA_ROOT"),
        "old storage environment removed"
    );
    require(
        !qEnvironmentVariableIsSet("SHADOW_PAINT_BRUSH_SETTINGS_FILE"),
        "old brush environment removed"
    );
    require(!qEnvironmentVariableIsSet("SHADOW_DESKTOP_SCAN_FOLDER"), "old pending import removed");
    QObject::connect(&endpoint, &StorageProfileSwitchEndpoint::abandoned, &app, [&] {
        app.exit(EXIT_FAILURE);
    });
    QObject::connect(&endpoint, &StorageProfileSwitchEndpoint::activate, &app, [&] {
        QFile marker(root + "/activated");
        require(marker.open(QIODevice::WriteOnly), "activation marker");
        marker.write(profile->id().toUtf8());
        marker.close();
        endpoint.confirmActivated();
        QTimer::singleShot(100, &app, &QCoreApplication::quit);
    });
    endpoint.ready(profile->info());
    QTimer::singleShot(10'000, &app, [&] { app.exit(EXIT_FAILURE); });
    return app.exec();
}
void contracts() {
    QTemporaryDir temp;
    const QString base = QFileInfo(temp.path()).canonicalFilePath();
    StorageProfileRegistry registry(base + "/daily", base + "/registry");
    QString error;
    auto target = registry.create("Target", base, &error);
    require(target.has_value(), "target fixture");
    qputenv("SHADOW_DESKTOP_DATA_ROOT", "old-profile");
    qputenv("SHADOW_PAINT_BRUSH_SETTINGS_FILE", "old-brush.ini");
    qputenv("SHADOW_DESKTOP_SCAN_FOLDER", "old-import-request");
    StorageProfileRestart restart;
    bool activated = false;
    QString failure;
    QObject::connect(&restart, &StorageProfileRestart::activated, [&] { activated = true; });
    QObject::connect(&restart, &StorageProfileRestart::failed, [&](const QString& value) {
        failure = value;
    });
    if (!restart.start(
            *target,
            registry.argumentsFor(*target),
            QCoreApplication::applicationFilePath(),
            &error
        ))
        throw std::runtime_error(error.toStdString());
    QElapsedTimer elapsed;
    elapsed.start();
    while (!activated && failure.isEmpty() && elapsed.elapsed() < 10'000)
        QTest::qWait(10);
    require(activated && failure.isEmpty(), "real child handshake activates exact target");
    require(
        QFileInfo::exists(target->root + "/activated"),
        "child only activates after parent admission"
    );
    QTest::qWait(200);
    StorageProfileLaunch held_launch;
    held_launch.daily_root = registry.dailyRoot();
    held_launch.arguments = {"Shadow"};
    held_launch.arguments.append(registry.argumentsFor(*target));
    auto held = StorageProfile::open(held_launch, &error);
    require(held != nullptr, "target child completed");
    StorageProfileRestart competing;
    failure.clear();
    QObject::connect(&competing, &StorageProfileRestart::failed, [&](const QString& value) {
        failure = value;
    });
    require(
        competing.start(
            *target,
            registry.argumentsFor(*target),
            QCoreApplication::applicationFilePath(),
            &error
        ),
        "competing child attempted"
    );
    elapsed.restart();
    while (failure.isEmpty() && elapsed.elapsed() < 10'000)
        QTest::qWait(10);
    require(
        failure.contains("already open"),
        "child startup error returns without target activation"
    );
    require(
        !StorageProfile::open(held_launch, &error),
        "failed restart does not release existing profile owner"
    );
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains("--profile-switch-server"))
            return child(app);
        contracts();
        std::cout << "profile restart contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
