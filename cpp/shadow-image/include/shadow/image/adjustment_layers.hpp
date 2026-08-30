#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shadow::image {

// A normalized selection shape owned by one adjustment-layer instance. It is
// intentionally independent from AdjustmentNode: a complete Grade Node still
// owns all of its color/tone controls, while this value only describes where
// its before/after blend is visible.
enum class LocalMaskKind : std::uint8_t {
    linear_gradient,
    radial_gradient,
    brush,
    luminance_range,
    color_range,
    managed_raster,
};

// Persisted managed-mask samples use a portable byte contract rather than a
// native float buffer. Gray16Float stores tightly packed little-endian
// IEEE-754 binary16 samples. Both encodings represent finite coverage in
// [0, 1].
enum class ManagedRasterMaskEncoding : std::uint8_t {
    gray8,
    gray16_float,
};

inline constexpr std::size_t maximum_composite_local_mask_components = 8U;
inline constexpr std::size_t maximum_composite_local_mask_raster_bytes = 64U * 1024U * 1024U;

enum class LocalMaskComponentOperation : std::uint8_t {
    base,
    add,
    subtract,
    intersect,
};

struct ManagedRasterMask final {
    Dimensions raster_dimensions;
    // The original-image coordinate extent that produced this raster. Runtime
    // sampling remains normalized so warm previews and full-resolution tiles
    // share the same placement without resampling this immutable payload.
    Dimensions coordinate_dimensions;
    ManagedRasterMaskEncoding encoding = ManagedRasterMaskEncoding::gray8;
    std::vector<std::uint8_t> samples;
};

struct LocalMaskPoint final {
    double x = 0.0;
    double y = 0.0;
    bool begins_stroke = false;
};

struct LocalMaskComponent;

struct LocalMask final {
    LocalMaskKind kind = LocalMaskKind::linear_gradient;
    // Geometry uses x0/y0/x1/y1 directly. Condition masks keep the fixed
    // cross-language record compact: luminance maps lower/upper to x0/x1;
    // color maps hue/360 and half-width/180 to x0/x1; managed rasters map
    // signed expansion/contraction to radius_y and edge softness to feather.
    // Presentation layers expose semantic names rather than these transport
    // slots.
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 1.0;
    double y1 = 0.0;
    double radius_x = 0.0;
    double radius_y = 0.0;
    double feather = 0.0;
    bool invert = false;
    std::vector<LocalMaskPoint> points;
    std::optional<ManagedRasterMask> managed_raster;
    // Empty preserves the exact legacy single-leaf contract. A non-empty
    // vector is one bounded ordered composition; this outer mask's `invert`
    // applies once after composition and all other outer leaf slots are ignored.
    std::vector<LocalMaskComponent> components;
};

struct LocalMaskComponent final {
    LocalMaskComponentOperation operation = LocalMaskComponentOperation::base;
    bool enabled = true;
    LocalMask mask;
};

// A sequential Grade Node layer. The first implementation supports only
// Normal blending; opacity and an optional spatial mask define the mix between
// the image before and after the enclosed adjustment chain.
struct AdjustmentLayer final {
    std::string layer_id;
    bool enabled = true;
    double opacity = 1.0;
    std::optional<LocalMask> mask;
    std::vector<AdjustmentNode> nodes;
};

// Executes complete Grade Node layers. Unmasked, fully opaque layers take the
// same direct node path; masked layers render a temporary result then blend it
// with the incoming image in original normalized coordinates. This preserves
// exact placement between warm proxies and independently requested detail
// tiles.
[[nodiscard]] FloatRgbImage execute_adjustment_layers(
    const FloatRgbImage& input,
    std::span<const AdjustmentLayer> layers,
    AdjustmentExecutionContext context = {}
);

} // namespace shadow::image
