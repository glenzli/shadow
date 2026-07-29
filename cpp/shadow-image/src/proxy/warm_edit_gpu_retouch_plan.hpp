#pragma once

#include "warm_edit_gpu_kernel_contract.hpp"

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace shadow::image::detail {

struct WarmRetouchRegion final {
    WarmRetouchRegionParameters parameters;
    std::size_t capsule_offset_bytes = 0U;
    std::size_t cell_offset_bytes = 0U;
    std::size_t reference_offset_bytes = 0U;
};

// Authored Repair/Clone strokes are sparse geometry, not rasters. This plan lowers clone
// regions into one compact, cacheable capsule/grid payload while retaining region order.
// The Metal encoder can then execute each region from the previous region's immutable input
// without scanning every authored point for every pixel.
struct WarmRetouchStage final {
    std::vector<WarmRetouchRegion> regions;
    std::vector<WarmRetouchWord> packed_geometry;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] std::optional<WarmRetouchStage> prepare_warm_retouch_stage(
    const SpotHealAdjustment& adjustment,
    Dimensions dimensions,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y,
    AdjustmentExecutionContext context
);

} // namespace shadow::image::detail
