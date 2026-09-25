#pragma once

#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

namespace desktop_backend_projection {

/// Converts the complete desktop Grade Stack and edit-history boundary
/// without letting individual backend workflows reinterpret wire vectors,
/// masks, retouch paths, geometry, identities, or version metadata.
[[nodiscard]] shadow::desktop::FfiEditPreviewPolicy
ffi_edit_preview_policy(EditPreviewPolicy policy);
[[nodiscard]] shadow::desktop::FfiGradeNode ffi_grade_node(const BackendGradeNode& source);
[[nodiscard]] BackendGradeNode grade_node(const shadow::desktop::FfiGradeNode& source);
[[nodiscard]] BackendSharedGradeNode
shared_grade_node(const shadow::desktop::FfiSharedGradeNode& source);
[[nodiscard]] shadow::desktop::FfiPhotoFoundationSettings
ffi_foundation(const BackendGradeStack& source);
[[nodiscard]] shadow::desktop::FfiEditSettings ffi_grade_stack(const BackendGradeStack& source);
[[nodiscard]] BackendGradeStack grade_stack(const shadow::desktop::FfiEditSettings& source);
[[nodiscard]] BackendPhotoEditState edit_state(const shadow::desktop::FfiPhotoEditState& source);

} // namespace desktop_backend_projection
