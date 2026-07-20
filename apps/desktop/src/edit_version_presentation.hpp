#pragma once

#include "desktop_backend.hpp"

#include <QString>

namespace EditVersionPresentation {

[[nodiscard]] QString displayName(const BackendEditVersion& version);
[[nodiscard]] QString changeSummary(const BackendEditVersion& version);
[[nodiscard]] QString parentSummary(const BackendEditVersion& version);

} // namespace EditVersionPresentation
