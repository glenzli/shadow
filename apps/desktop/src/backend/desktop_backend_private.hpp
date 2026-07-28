#pragma once

#include "../desktop_backend.hpp"
#include "../folder_scan_backend.hpp"
#include "export_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <utility>

// Shared only by DesktopBackend member translation units. The public facade
// remains pimpl-based; workflow implementations receive no wider dependency.
struct DesktopBackend::Impl final {
    explicit Impl(rust::Box<shadow::desktop::DesktopSession> value)
        : session(std::move(value)),
          folder_scan_backend(*session),
          export_backend(*session) {}

    rust::Box<shadow::desktop::DesktopSession> session;
    FolderScanBackend folder_scan_backend;
    ExportBackend export_backend;
};
