#pragma once

#include <shadow/image/photo_structural_rendering.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace shadow::image::detail {

/// R8 presentation of a scalar original-image raster after the same fixed
/// structural order used by RGB: optional Liquify, then mandatory Canvas.
///
/// The source remains float until the final fused sample is quantized. This is
/// used for paired local-mask coverage so its geometry cannot drift from the
/// preview frame it annotates.
struct PhotoStructuralScalarR8 final {
    Dimensions dimensions;
    std::uint32_t row_stride_bytes = 0U;
    std::vector<std::uint8_t> samples;
};

[[nodiscard]] std::optional<PhotoStructuralScalarR8>
apply_photo_structural_scalar_r8(
    Dimensions source_dimensions,
    std::span<const float> source_samples,
    const PreparedPhotoStructuralRendering& structural,
    std::stop_token cancellation
);

} // namespace shadow::image::detail
