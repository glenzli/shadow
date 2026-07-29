#pragma once

#include "edit_preview_frame.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <memory>

// Adapts the second CXX Box boundary to the presentation-neutral frame
// interface. Only this owner depends on the generated Rust bridge.
[[nodiscard]] std::shared_ptr<const BackendEditPreviewFrame>
makeRustOwnedEditPreviewFrame(rust::Box<shadow::desktop::OwnedEditedPreview> owner);
