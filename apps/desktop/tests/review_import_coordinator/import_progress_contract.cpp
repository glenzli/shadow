#include "import_coordinator_fixture.hpp"

namespace review_import_test {

void run_import_progress_contracts() {
    auto state = std::make_shared<ImportBackendState>();
    state->report = {
        .folder_path = QStringLiteral("/tmp/import-finished"),
        .files_seen = 9,
        .supported_files = 8,
        .inserted = 4,
        .unchanged = 2,
        .needs_revalidation = 1,
        .decode_queued = 7,
        .decode_completed = 6,
        .decode_hard_failures = 1,
        .preview_failures = 2,
        .decode_cancelled = 0,
        .issue_count = 3,
    };
    ReviewImportCoordinator coordinator(operations(state));
    int terminal_refreshes = 0;
    QObject::connect(
        &coordinator,
        &ReviewImportCoordinator::terminalRefreshRequested,
        [&terminal_refreshes]() { ++terminal_refreshes; }
    );

    require(
        !coordinator.start(QUrl::fromLocalFile("/tmp/rejected"), false)
            && begun_count(state) == 0,
        "cross-workflow rejection does not allocate a scan identity"
    );
    require(
        coordinator.start(QUrl::fromLocalFile("/tmp/import"), true),
        "an admitted local folder starts"
    );
    wait_for_scan_entry(state);
    require(
        coordinator.scanning()
            && coordinator.folderPath() == QStringLiteral("/tmp/import"),
        "the coordinator owns the active folder and running state"
    );

    set_progress(
        state,
        {
            .scan_id = 1,
            .update_sequence = 4,
            .files_seen = 7,
            .supported_files = 6,
            .inserted = 2,
            .unchanged = 1,
            .needs_revalidation = 1,
            .decode_queued = 4,
            .preview_artifacts_ready = 2,
            .decode_completed = 2,
            .decode_hard_failures = 1,
            .preview_failures = 1,
            .decode_cancelled = 0,
            .skipped = 1,
            .issue_count = 2,
            .phase = BackendScanPhase::Discovering,
            .valid = true,
        }
    );
    wait_until(
        [&coordinator]() {
            return coordinator.progress()
                       .value(QStringLiteral("updateSequence"))
                       .toULongLong()
                == 4;
        },
        "the polling timer accepts the latest monotonic snapshot"
    );
    const QVariantMap progress = coordinator.progress();
    require(
        progress.value(QStringLiteral("cataloguedFiles")).toULongLong() == 4
            && progress.value(QStringLiteral("previewArtifactsReady"))
                       .toULongLong()
                == 2
            && progress.value(QStringLiteral("phase")).toString()
                == QStringLiteral("discovering"),
        "the stable QML map projects catalog and preview progress"
    );
    require(
        coordinator.takeStreamRefreshRequest(0, false)
            && !coordinator.takeStreamRefreshRequest(0, false)
            && !coordinator.takeStreamRefreshRequest(0, true),
        "a visible-prefix opportunity is consumed once and blocked by page work"
    );

    release_scan(state);
    wait_until(
        [&coordinator]() { return !coordinator.scanning(); },
        "the completed worker reaches a terminal state"
    );
    require(
        terminal_refreshes == 1
            && coordinator.folderPath()
                == QStringLiteral("/tmp/import-finished")
            && coordinator.progress()
                   .value(QStringLiteral("phase"))
                   .toString()
                == QStringLiteral("completed"),
        "one terminal refresh follows the authoritative report"
    );
    require(
        coordinator.readyStatusMessage(7, 7, false)
            .translated()
            .contains(QStringLiteral("7 / 7 loaded")),
        "ready status uses the terminal import summary"
    );
}

} // namespace review_import_test
