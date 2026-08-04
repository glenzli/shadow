#include "library_server_controller.hpp"

#include "desktop_backend.hpp"

#include <utility>

namespace {

[[nodiscard]] LibraryServerControllerOperations
backendOperations(const std::shared_ptr<DesktopBackend>& backend) {
    return {
        .snapshot = [backend] { return backend->libraryServerSnapshot(); },
        .start = [backend](
                     const BackendLibraryServerConfig& config
                 ) { return backend->startLibraryServer(config); },
        .stop = [backend] { return backend->stopLibraryServer(); },
        .reset_cache = [backend] { return backend->resetLibraryServerCache(); },
    };
}

} // namespace

LibraryServerController::LibraryServerController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    std::unique_ptr<SecretStore> secret_store,
    QObject* const parent
) :
    LibraryServerController(
        backendOperations(backend),
        isolated_settings_file,
        std::move(secret_store),
        parent
    ) {}
