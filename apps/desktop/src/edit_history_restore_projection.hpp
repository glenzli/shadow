#pragma once

#include "desktop_backend.hpp"

#include <QString>

#include <string>

namespace EditHistoryRestoreProjection {

[[nodiscard]] bool
gradeNodeListChanged(const BackendGradeStack& before, const BackendGradeStack& after);

[[nodiscard]] bool localMaskChanged(const BackendGradeNode& before, const BackendGradeNode& after);

[[nodiscard]] QString preferredGradeNodeForRestore(
    const std::string& history_key,
    const BackendGradeStack& restored,
    int previous_selected_index
);

} // namespace EditHistoryRestoreProjection
