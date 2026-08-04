#include "library_server_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

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
        .snapshot = [service] { return service->snapshot; },
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
                };
                return service->snapshot;
            },
        .stop =
            [service] {
                ++service->stops;
                service->snapshot.running = false;
                service->snapshot.local_address.clear();
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
            "start creates a bounded Keychain token without exposing it as a property"
        )) {
        return EXIT_FAILURE;
    }

    controller.stopServer();
    waitForIdle(controller);
    if (!require(!controller.running(), "stop publishes the terminal snapshot")
        || !require(service->stops == 1, "stop is admitted exactly once")
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
