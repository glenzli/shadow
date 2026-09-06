#include "library_server_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

class MemorySecretStore final : public SecretStore {
  public:
    [[nodiscard]] bool available() const noexcept override {
        return true;
    }

    [[nodiscard]] SecretStoreResult read(const QString&, const QString&) const override {
        return value_.isEmpty()
                   ? SecretStoreResult{.status = SecretStoreStatus::NotFound}
                   : SecretStoreResult{.status = SecretStoreStatus::Success, .value = value_};
    }

    [[nodiscard]] SecretStoreResult
    write(const QString&, const QString&, const QString& value) override {
        value_ = value;
        return {.status = SecretStoreStatus::Success};
    }

    [[nodiscard]] SecretStoreResult remove(const QString&, const QString&) override {
        value_.clear();
        return {.status = SecretStoreStatus::Success};
    }

  private:
    QString value_;
};

struct FakeService final {
    BackendLibraryServerSnapshot snapshot;
    BackendLibraryServerConfig last_config;
    int starts = 0;
    int stops = 0;
    int snapshot_reads = 0;
    bool fail_stop_after_transition = false;
};

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Library server controller contract failed: " << message << '\n';
    }
    return condition;
}

void waitForIdle(LibraryServerController& controller) {
    for (;;) {
        QCoreApplication::processEvents();
        if (!controller.busy()) {
            QCoreApplication::processEvents();
            if (!controller.busy()) {
                break;
            }
        }
    }
}

template <typename Predicate> [[nodiscard]] bool waitFor(Predicate predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        QCoreApplication::processEvents();
        if (elapsed.elapsed() > 2'000) {
            return false;
        }
        QThread::msleep(2);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir fixture;
    if (!fixture.isValid()) {
        return EXIT_FAILURE;
    }
    const QString photo_root = QDir(fixture.path()).filePath(QStringLiteral("photos"));
    QDir().mkpath(photo_root);
    const QString canonical_photo_root = QFileInfo(photo_root).canonicalFilePath();
    const QString settings_path = QDir(fixture.path()).filePath(QStringLiteral("settings.ini"));
    auto service = std::make_shared<FakeService>();
    service->snapshot.provider_mode = QStringLiteral("private");
    LibraryServerControllerOperations operations{
        .snapshot =
            [service] {
                ++service->snapshot_reads;
                if (service->snapshot.running
                    && service->snapshot.index_state == QStringLiteral("scanning")) {
                    service->snapshot.index_state = QStringLiteral("ready");
                    service->snapshot.photo_count = 31;
                    service->snapshot.discovered_file_count = 34;
                    service->snapshot.inspection_completed_count = 31;
                    service->snapshot.published_preview_count = 29;
                }
                return service->snapshot;
            },
        .start =
            [service](const BackendLibraryServerConfig& config) {
                service->last_config = config;
                ++service->starts;
                service->snapshot = {
                    .running = true,
                    .local_address = QStringLiteral("0.0.0.0:37641"),
                    .display_name = config.display_name,
                    .provider_mode = QStringLiteral("private"),
                    .photo_count = 27,
                    .cache_byte_len = 1'024,
                    .shared_root_count = static_cast<std::uint64_t>(config.share_roots.size()),
                    .serves_originals = config.serves_originals,
                    .index_state = QStringLiteral("scanning"),
                    .discovered_file_count = 12,
                    .inspection_completed_count = 7,
                    .published_preview_count = 6,
                };
                return service->snapshot;
            },
        .stop =
            [service] {
                ++service->stops;
                service->snapshot.running = false;
                service->snapshot.local_address.clear();
                if (service->fail_stop_after_transition) {
                    throw std::runtime_error("synthetic listener shutdown failure");
                }
                return service->snapshot;
            },
        .reset_cache =
            [service] {
                service->snapshot.cache_byte_len = 0;
                service->snapshot.photo_count = 0;
                return service->snapshot;
            },
    };
    LibraryServerController controller(
        std::move(operations),
        settings_path,
        std::make_unique<MemorySecretStore>()
    );
    waitForIdle(controller);

    if (!require(
            controller.addSharedFolder(QUrl::fromLocalFile(photo_root)),
            "an available local folder can be added"
        )
        || !require(
            controller.sharedFolders().size() == 1,
            "the configured folder is projected to QML"
        )) {
        return EXIT_FAILURE;
    }

    controller.setDisplayName(QStringLiteral("Studio Mac"));
    controller.setPort(45'321);
    controller.setServesOriginals(false);
    controller.startServer();
    waitForIdle(controller);
    if (!require(controller.running(), "start publishes the running snapshot")
        || !require(service->starts == 1, "start is admitted exactly once")
        || !require(
            service->last_config.bind_address == QStringLiteral("0.0.0.0:45321"),
            "the configured port reaches the backend"
        )
        || !require(
            service->last_config.share_roots == QStringList{canonical_photo_root},
            "the exact configured root reaches the backend"
        )
        || !require(
            !service->last_config.serves_originals,
            "the original-download permission reaches the backend"
        )
        || !require(
            service->last_config.authorization.size() >= 32,
            "start creates a bounded local token without exposing it as a property"
        )
        || !require(controller.indexing(), "start projects background indexing state")
        || !require(
            controller.statusCode() == QStringLiteral("indexing"),
            "start explains that the published generation remains available"
        )
        || !require(
            controller.discoveredFileCount() == 12 && controller.inspectionCompletedCount() == 7
                && controller.publishedPreviewCount() == 6,
            "background progress counters are projected independently"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            waitFor([&controller] { return !controller.indexing(); }),
            "controller polls until background indexing reaches a terminal state"
        )
        || !require(service->snapshot_reads >= 1, "indexing performs a bounded status refresh")
        || !require(controller.photoCount() == 31, "completed generation updates the photo count")
        || !require(
            controller.statusCode() == QStringLiteral("running"),
            "completed indexing returns to the ordinary running state"
        )) {
        return EXIT_FAILURE;
    }

    service->snapshot.index_state = QStringLiteral("failed");
    service->snapshot.index_diagnostic = QStringLiteral("synthetic scan failure");
    controller.refresh();
    waitForIdle(controller);
    if (!require(controller.running(), "a failed reindex does not stop sharing")
        || !require(
            controller.statusCode() == QStringLiteral("indexing-failed"),
            "failed reindex has a distinct non-terminal status"
        )
        || !require(
            controller.indexDiagnosticText() == QStringLiteral("synthetic scan failure"),
            "failed reindex projects its bounded diagnostic"
        )) {
        return EXIT_FAILURE;
    }

    service->fail_stop_after_transition = true;
    controller.stopServer();
    waitForIdle(controller);
    if (!require(!controller.running(), "failed stop refreshes the terminal backend snapshot")
        || !require(service->stops == 1, "stop is admitted exactly once")
        || !require(
            controller.statusCode() == QStringLiteral("operation-failed"),
            "failed stop preserves its operation diagnostic"
        )
        || !require(
            controller.diagnosticText().contains(QStringLiteral("synthetic listener")),
            "failed stop explains the teardown failure"
        )) {
        return EXIT_FAILURE;
    }

    service->fail_stop_after_transition = false;
    controller.startServer();
    waitForIdle(controller);
    controller.stopServer();
    waitForIdle(controller);
    if (!require(!controller.running(), "a recovered controller can start and stop again")
        || !require(service->stops == 2, "the second stop is admitted exactly once")
        || !require(controller.removeSharedFolder(0), "a stopped server permits root removal")) {
        return EXIT_FAILURE;
    }

    QSettings persisted(settings_path, QSettings::IniFormat);
    return require(
               persisted.value(QStringLiteral("library-server/shared_folders"))
                   .toStringList()
                   .isEmpty(),
               "folder removal is durable"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
