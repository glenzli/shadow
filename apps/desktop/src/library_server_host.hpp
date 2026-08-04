#pragma once

#include "library_server_controller.hpp"

#include <memory>

namespace shadow::desktop {
struct LibraryServerHost;
}

/// Thin Qt-side owner for the standalone Rust Library-server host.
///
/// It deliberately exposes the same operation bundle consumed by the existing
/// controller, so the standalone app and Shadow's embedded pane cannot drift
/// into separate lifecycle or validation policies.
class LibraryServerHost final {
  public:
    explicit LibraryServerHost(const QString& storage_root);
    ~LibraryServerHost();

    LibraryServerHost(const LibraryServerHost&) = delete;
    LibraryServerHost& operator=(const LibraryServerHost&) = delete;

    [[nodiscard]] LibraryServerControllerOperations operations();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
