#pragma once

#include "history_types.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

namespace desktop_backend_projection {

[[nodiscard]] shadow::desktop::FfiHistoryCursor
ffi_history_cursor(const BackendHistoryCursor& source);
[[nodiscard]] BackendPhotoHistoryPage
photo_history_page(const shadow::desktop::FfiPhotoHistoryPage& source);
[[nodiscard]] BackendLibraryHistoryPage
library_history_page(const shadow::desktop::FfiLibraryHistoryPage& source);
[[nodiscard]] BackendLibraryHistoryRefPage
library_history_ref_page(const shadow::desktop::FfiLibraryHistoryRefPage& source);

} // namespace desktop_backend_projection
