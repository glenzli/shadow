#include "storage_profile_controller.hpp"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
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

void archiveContracts() {
    QTemporaryDir fixture;
    const QString base = QFileInfo(fixture.path()).canonicalFilePath();
    StorageProfileRegistry registry(base + "/daily", base + "/registry");
    StorageProfileLaunch launch;
    launch.daily_root = registry.dailyRoot();
    launch.arguments = {"Shadow", "--profile-root", base + "/current"};
    QString error;
    auto current = StorageProfile::open(launch, &error);
    require(current != nullptr, "open archive controller fixture");
    bool quiet = false, saved = false, suspended = false;
    int requests = 0, cancellations = 0;
    StorageProfileArchiveRequest request;
    StorageProfileController controller(
        *current,
        registry,
        "debug",
        {
            .blocker = [] { return QString{}; },
            .suspend_background = [&](bool value) { suspended = value; },
            .jobs_quiet = [&] { return quiet; },
            .prepare_editor = [&] { return saved; },
            .restart = {},
            .archive_blocker = [] { return QString{}; },
            .archive =
                [&](const StorageProfileArchiveRequest& value) {
                    request = value;
                    ++requests;
                },
            .cancel_archive = [&] { ++cancellations; },
        }
    );
    controller.backupLibrary(QUrl::fromLocalFile(base));
    require(
        controller.busy() && suspended && requests == 0,
        "backup waits for quiet jobs and durable edit save"
    );
    quiet = true;
    QTest::qWait(120);
    require(controller.busy() && requests == 0, "unsaved edit cannot enter snapshot");
    controller.editorSaveFailed("disk full");
    QCoreApplication::processEvents();
    require(
        !controller.busy() && !suspended && requests == 0,
        "failed save preserves the current library and prevents backup"
    );
    saved = true;
    controller.backupLibrary(QUrl::fromLocalFile(base));
    require(
        requests == 1 && request.action == 0 && request.source == current->root()
            && request.expected_id == current->id(),
        "archive owns exact current source identity"
    );
    controller.cancelSwitch();
    require(
        cancellations == 1 && controller.busy() && suspended,
        "cancel waits for worker cleanup before resuming writes"
    );
    controller.archiveFailed("cancelled");
    QCoreApplication::processEvents();
    require(
        !controller.busy() && !suspended && controller.errorText().isEmpty(),
        "cancelled archive leaves the session usable"
    );
    controller.createTestCopy(QUrl::fromLocalFile(base), "Copy");
    require(
        request.action == 2 && controller.busy(),
        "test copy uses the same save and pause boundary"
    );
    auto copy = registry.create("Copy", base, &error);
    require(copy.has_value(), "fixture completed copy");
    controller.archiveCompleted(copy->root);
    require(
        controller.busy(),
        "synchronous completion remains owned throughout closeReady delivery"
    );
    QCoreApplication::processEvents();
    require(
        !controller.busy() && !suspended && controller.resultRoot() == copy->root,
        "new copy returns to the old session with a reviewable result"
    );
    require(
        registry.resolveLaunch(launch, "debug", &error)->arguments.contains(current->root()),
        "creating a copy does not switch or rewrite launch history"
    );
}
void contracts() {
    QTemporaryDir fixture;
    const QString base = QFileInfo(fixture.path()).canonicalFilePath();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, base + "/legacy");
    StorageProfileRegistry registry(base + "/daily", base + "/registry");
    StorageProfileLaunch launch;
    launch.daily_root = registry.dailyRoot();
    launch.debug_build = true;
    launch.arguments = {"Shadow"};
    QString error;
    auto current = StorageProfile::open(launch, &error);
    require(current != nullptr, "open fixture current");
    require(registry.remember(current->info(), "debug", &error), "remember current");
    auto target = registry.create("Experiment", base, &error);
    require(
        target && registry.remember(*target, {}, &error),
        "create target without changing launch"
    );
    bool quiet = false, prepared = false, suspended = false;
    int starts = 0, switches = 0;
    QString blocker;
    QStringList started_args;
    StorageProfileController controller(
        *current,
        registry,
        "debug",
        {
            .blocker = [&] { return blocker; },
            .suspend_background = [&](bool value) { suspended = value; },
            .jobs_quiet = [&] { return quiet; },
            .prepare_editor = [&] { return prepared; },
            .restart =
                [&](const StorageProfileInfo& info, const QStringList& args) {
                    require(info.id == target->id, "exact target identity");
                    ++starts;
                    started_args = args;
                },
        }
    );
    QObject::connect(&controller, &StorageProfileController::switched, [&] { ++switches; });
    controller.switchLibrary(target->root, target->id);
    require(
        controller.busy() && controller.canCancel() && suspended && starts == 0,
        "switch waits for current operations and suspends background admission"
    );
    controller.renameCurrent("Must not change");
    require(current->info().name() != "Must not change", "mutations blocked during switch");
    controller.cancelSwitch();
    require(!controller.busy() && !suspended && starts == 0, "cancel waiting leaves session live");
    blocker = "Save composition";
    controller.switchLibrary(target->root, target->id);
    require(
        !controller.busy() && controller.errorText() == blocker,
        "unsaved temporary authored output blocks switch"
    );
    blocker.clear();
    quiet = true;
    controller.switchLibrary(target->root, target->id);
    require(
        controller.busy() && !controller.canCancel() && starts == 0,
        "durable save must precede restart"
    );
    controller.editorSaveFailed("disk full");
    require(controller.busy(), "save failure delivery remains owned during normal close listeners");
    QCoreApplication::processEvents();
    require(
        !controller.busy() && !suspended && controller.errorText().contains("disk full")
            && starts == 0,
        "save failure preserves current editing session"
    );
    require(!StorageProfile::open(launch, &error), "old writer lock is retained after failure");
    error.clear();
    auto remembered = registry.resolveLaunch(launch, "debug", &error);
    require(
        remembered && remembered->arguments.contains(current->id()),
        "failed save preserves old launch choice"
    );
    prepared = true;
    controller.switchLibrary(target->root, target->id);
    require(
        starts == 1 && controller.busy() && suspended,
        "saved current starts target while retaining current owner"
    );
    require(
        started_args.contains("--profile-id") && started_args.contains(target->id)
            && started_args.contains("--profile-launch-context") && started_args.contains("debug"),
        "restart freezes target and launch lane"
    );
    controller.restartFailed("target startup failed");
    QCoreApplication::processEvents();
    require(
        !controller.busy() && !suspended && switches == 0,
        "startup failure stays in current session"
    );
    controller.editorReady();
    require(starts == 1, "late save callback cannot revive failed request");
    prepared = false;
    controller.switchLibrary(target->root, target->id);
    controller.editorReady();
    require(starts == 2 && switches == 0, "ready save is not target activation");
    controller.restartActivated();
    controller.restartFailed("late failure");
    QCoreApplication::processEvents();
    require(
        switches == 1 && current->kind() == "debug",
        "only acknowledged activation asks old app to exit"
    );
    require(
        controller.busy() && suspended,
        "activation keeps old background work suspended until exit"
    );
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    try {
        contracts();
        archiveContracts();
        std::cout << "profile controller contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
