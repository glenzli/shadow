#pragma once
#include "retouch_source_transform.hpp"

namespace shadow::image::detail {
// Conservative backwards dependencies through ordered, spatially bounded
// repair regions. Avoids charging every donor displacement to unrelated pixels.
RetouchSourceReach retouch_dependency_reach(
    const SpotHealAdjustment& adjustment,
    double scale_x,
    double scale_y,
    Dimensions dimensions
);
} // namespace shadow::image::detail
