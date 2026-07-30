#pragma once

#include <shadow/image/adjustment_layers.hpp>

#include <vector>

namespace shadow::image::detail {

// Validates the complete portable raster contract, including the byte budget,
// exact tightly packed sample count, and finite [0, 1] binary16 values.
void validate_managed_raster_mask(const ManagedRasterMask& mask);

// Samples immutable mask coverage in normalized original-image coordinates.
// Bilinear filtering uses pixel-center alignment and clamps at raster edges so
// preview and detail-tile boundaries cannot diverge.
[[nodiscard]] double sample_managed_raster_mask(
    const ManagedRasterMask& mask,
    double normalized_x,
    double normalized_y
) noexcept;

// Temporary, renderer-owned refinement of immutable managed coverage. Samples
// are float only while one render plan is prepared; the Recipe reference and
// stored provider bytes remain unchanged.
struct RefinedManagedRasterMask final {
    Dimensions dimensions;
    std::vector<float> samples;

    [[nodiscard]] bool valid() const noexcept;
};

// Applies signed edge expansion followed by symmetric feathering. Both
// controls use normalized authoring units; 1.0 maps to a bounded fraction of
// the stored raster's shorter edge. The implementation is separable and
// enforces a fixed peak scratch budget before allocating.
[[nodiscard]] RefinedManagedRasterMask
refine_managed_raster_mask(const ManagedRasterMask& mask, double expansion, double feather);

[[nodiscard]] double sample_refined_managed_raster_mask(
    const RefinedManagedRasterMask& mask,
    double normalized_x,
    double normalized_y
) noexcept;

} // namespace shadow::image::detail
