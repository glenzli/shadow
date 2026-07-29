#pragma once

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"

#include <optional>
#include <string>

namespace shadow::image::detail {

// Portable lowering for the post-edit geometry sampler. The public PhotoGeometry layout remains
// the rounding authority; this plan only seals its validated coordinates into the Metal ABI.
struct WarmGpuGeometryPlan final {
    WarmPhotoGeometryParameters parameters;
    Dimensions output_dimensions;
    double output_level_zero_to_raster_scale_x = 1.0;
    double output_level_zero_to_raster_scale_y = 1.0;

    [[nodiscard]] bool valid() const noexcept;
};

struct WarmGpuGeometryPreparation final {
    std::optional<WarmGpuGeometryPlan> plan;
    std::string diagnostic;
};

[[nodiscard]] WarmGpuGeometryPreparation prepare_warm_gpu_geometry_plan(
    Dimensions resident_dimensions,
    std::uint32_t input_row_floats,
    double input_level_zero_to_raster_scale_x,
    double input_level_zero_to_raster_scale_y,
    const WarmEditGpuGeometryContext& context
);

} // namespace shadow::image::detail
