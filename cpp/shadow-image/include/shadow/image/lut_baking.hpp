#pragma once

#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/lut.hpp>

#include <span>
#include <stop_token>
#include <string>

namespace shadow::image {

/// A sampled linear-sRGB -> linear-sRGB transform on [0, 1]^3. The error is
/// measured against independent deterministic probes, not a bound over all RGB.
struct CubeLutBakeResult final {
    CubeLut3D lut;
    double maximum_absolute_error = 0.0;
    double root_mean_square_error = 0.0;
    std::uint32_t probe_count = 0;
};

/// Reuses the ordinary CPU Grade Node executor. Rejects enabled spatial/source
/// operations and masks; the caller must explicitly project and report losses.
/// Node opacity is preserved. Output values are finite and may exceed [0, 1].
[[nodiscard]] CubeLutBakeResult bake_cube_lut(
    std::span<const AdjustmentLayer> layers,
    std::uint16_t size,
    std::stop_token cancellation = {}
);

/// Deterministic standard .cube text. The input/output-space comments are
/// informational; other programs must select the declared color space.
[[nodiscard]] std::string serialize_baked_cube_lut(const CubeLutBakeResult& baked);

/// Display-reference LUT card: encoded sRGB -> linear sRGB -> LUT -> the same
/// gamut mapping/OETF as ordinary raster editing. No RAW scene curve is added.
[[nodiscard]] DisplayRgb8Image render_cube_lut_reference(
    const CubeLut3D& lut,
    Dimensions dimensions,
    std::span<const float> encoded_srgb
);

} // namespace shadow::image
