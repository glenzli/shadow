#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

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
};

struct LocalMaskPoint final {
    double x = 0.0;
    double y = 0.0;
    bool begins_stroke = false;
};

struct LocalMask final {
    LocalMaskKind kind = LocalMaskKind::linear_gradient;
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 1.0;
    double y1 = 0.0;
    double radius_x = 0.0;
    double radius_y = 0.0;
    double feather = 0.0;
    bool invert = false;
    std::vector<LocalMaskPoint> points;
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
