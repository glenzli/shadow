#pragma once

#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/working_rgb.hpp>

#include "managed_raster_mask.hpp"
#include "working_color_math.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace shadow::image::detail {

// Prepared once per mask evaluation so geometry scaling and working-space conversion cannot drift
// between layer blending and the optional preview-coverage capture.
struct PreparedLocalMaskCoverage final {
    const LocalMask* mask = nullptr;
    double brush_scale_x = 1.0;
    double brush_scale_y = 1.0;
    std::optional<WorkingSpaceTransform> color_transform;
    std::optional<RefinedManagedRasterMask> refined_managed_raster;
};

[[nodiscard]] PreparedLocalMaskCoverage prepare_local_mask_coverage(
    const LocalMask& mask,
    const FloatRgbImage& source,
    Dimensions full_dimensions
);

[[nodiscard]] double local_mask_coverage_at(
    const PreparedLocalMaskCoverage& prepared,
    double normalized_x,
    double normalized_y,
    const Vector3& source_rgb
) noexcept;

// Scene-geometry coverage before crop/orientation. Samples remain float until the paired preview
// geometry has run; only the final public payload is quantized to R8.
struct LocalMaskCoverageRaster final {
    Dimensions dimensions;
    std::vector<float> samples;

    [[nodiscard]] bool valid() const noexcept;
};

struct LocalMaskCoverageR8 final {
    Dimensions dimensions;
    std::uint32_t row_stride_bytes = 0U;
    std::vector<std::uint8_t> samples;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] std::optional<LocalMaskCoverageRaster> render_local_mask_coverage(
    const FloatRgbImage& source,
    const LocalMask& mask,
    AdjustmentExecutionContext context,
    Dimensions full_dimensions,
    std::stop_token cancellation
);

[[nodiscard]] std::optional<LocalMaskCoverageR8> apply_local_mask_coverage_geometry(
    const LocalMaskCoverageRaster& source,
    const PhotoGeometry& geometry,
    std::stop_token cancellation
);

struct AdjustmentLayersWithMaskCoverage final {
    FloatRgbImage pixels;
    std::optional<LocalMaskCoverageRaster> mask_coverage;
};

// Executes the normal sequential layer transaction while optionally capturing one target mask
// from that layer's pre-adjustment input. A supplied out-of-range target is a typed request error;
// a valid layer without a mask completes with no coverage.
[[nodiscard]] std::optional<AdjustmentLayersWithMaskCoverage>
execute_adjustment_layers_with_mask_coverage(
    const FloatRgbImage& input,
    std::span<const AdjustmentLayer> layers,
    std::optional<std::uint32_t> target_layer_index,
    AdjustmentExecutionContext context,
    std::stop_token cancellation
);

} // namespace shadow::image::detail
