#include "backend/native_path_input.hpp"
#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <concepts>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

static_assert(std::same_as<
    decltype(std::declval<const DesktopBackend&>().beginFolderScan(
        std::uint64_t{}
    )),
    void
>);
static_assert(std::same_as<
    decltype(std::declval<const DesktopBackend&>().scanFolder(
        QString{},
        std::uint64_t{}
    )),
    BackendScanReport
>);
static_assert(std::same_as<
    decltype(std::declval<const DesktopBackend&>().scanProgress(
        std::uint64_t{}
    )),
    BackendScanProgress
>);
static_assert(std::same_as<
    decltype(std::declval<const DesktopBackend&>().cancelFolderScan(
        std::uint64_t{}
    )),
    bool
>);

namespace {

void require(const bool condition, const std::string& contract) {
    if (!condition) {
        std::cerr << "folder-scan backend contract changed: "
                  << contract << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void cancellation_and_completion_remain_one_scan_identity_lifecycle() {
    QTemporaryDir root;
    require(root.isValid(), "temporary data root");
    const QString import_path = root.filePath(QStringLiteral("photos"));
    require(QDir{}.mkpath(import_path), "empty import folder");

    auto session = shadow::desktop::open_desktop_session(
        native_path_input::path(root.filePath(QStringLiteral("catalog.sqlite"))),
        native_path_input::path(root.filePath(QStringLiteral("cache")))
    );
    const FolderScanBackend scans(*session);

    require(!scans.scanProgress(41).valid, "unknown scan is absent");

    scans.beginFolderScan(41);
    BackendScanProgress progress = scans.scanProgress(41);
    require(progress.valid, "begun scan is visible");
    require(progress.scan_id == 41, "begun scan identity");
    require(
        progress.phase == BackendScanPhase::Discovering,
        "begun scan phase"
    );

    require(scans.cancelFolderScan(41), "first cancellation wins");
    require(!scans.cancelFolderScan(41), "repeat cancellation is a no-op");
    progress = scans.scanProgress(41);
    require(
        progress.phase == BackendScanPhase::Cancelling,
        "prepared cancellation remains observable"
    );

    const BackendScanReport cancelled = scans.scanFolder(import_path, 41);
    require(cancelled.cancelled, "cancelled report");
    require(cancelled.files_seen == 0, "cancelled scan did no discovery");
    require(
        scans.scanProgress(41).phase == BackendScanPhase::Cancelled,
        "cancelled terminal phase"
    );

    scans.beginFolderScan(42);
    const BackendScanReport completed = scans.scanFolder(import_path, 42);
    require(!completed.cancelled, "replacement scan completed");
    require(
        QDir::cleanPath(completed.folder_path) == QDir::cleanPath(import_path),
        "reported folder identity"
    );
    require(completed.files_seen == 0, "empty folder files seen");
    require(completed.supported_files == 0, "empty folder supported files");
    require(completed.inserted == 0, "empty folder inserted files");
    require(completed.decode_queued == 0, "empty folder decode queue");
    require(completed.issue_count == 0, "empty folder issues");

    progress = scans.scanProgress(42);
    require(progress.valid, "completed progress is visible");
    require(progress.scan_id == 42, "completed scan identity");
    require(
        progress.phase == BackendScanPhase::Completed,
        "completed terminal phase"
    );
    require(progress.update_sequence > 0, "monotonic progress sequence");
    require(
        progress.supported_files == completed.supported_files,
        "report/progress supported count"
    );
    require(
        progress.inserted == completed.inserted,
        "report/progress inserted count"
    );
    require(!scans.scanProgress(41).valid, "replaced scan becomes stale");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        cancellation_and_completion_remain_one_scan_identity_lifecycle();
    } catch (const std::exception& error) {
        std::cerr << "folder-scan production path failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
