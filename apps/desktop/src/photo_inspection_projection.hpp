#pragma once

#include "desktop_backend.hpp"

namespace shadow::desktop {
struct FfiPhotoInspection;
}

/// Projects the complete Rust selected-photo inspection contract into the
/// desktop DTO consumed by ReviewController.
///
/// Keeping this mapping in one production owner lets contract tests exercise
/// the real projection with sentinel values instead of duplicating it.
[[nodiscard]] BackendPhotoInspection project_photo_inspection(
    const shadow::desktop::FfiPhotoInspection& source
);
