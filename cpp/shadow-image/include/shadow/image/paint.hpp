#pragma once
#include <array>
#include <cstdint>
#include <shadow/image/working_rgb.hpp>
#include <vector>

namespace shadow::image {
struct AdjustmentExecutionContext;
// Original normalized geometry. Radius uses the authored shorter side.
struct PaintPoint final {
    double x = 0.0;
    double y = 0.0;
    double pressure = 1.0;
};
struct PaintStroke final {
    std::vector<PaintPoint> points;
    double radius = 0.01;
    double hardness = 0.0;
    double opacity = 1.0;
    double flow = 0.1;
    std::array<double, 3> color{}; // unassociated display-sRGB
    bool erase = false;
};
// 0 Normal, 1 Oklab Color, 2 Oklab-lightness Soft Light.
struct PaintLayerAdjustment final {
    std::uint32_t coordinate_width = 0;
    std::uint32_t coordinate_height = 0;
    std::uint8_t blend = 0;
    double opacity = 1.0;
    std::vector<PaintStroke> strokes;
};
namespace detail {
// Tightly packed premultiplied working-RGB + coverage, restricted to touched
// pixels of this execution tile. No input image pixels are read to prepare it.
struct PreparedPaintOverlay final {
    std::uint32_t left = 0, top = 0, width = 0, height = 0;
    std::vector<std::array<float, 4>> pixels;
};
void validate_paint_layer(const PaintLayerAdjustment& layer);
PreparedPaintOverlay prepare_paint_overlay(
    const PaintLayerAdjustment& layer,
    const FloatRgbImage& image,
    const AdjustmentExecutionContext& context
);
void apply_paint_layer(
    FloatRgbImage& image,
    const PaintLayerAdjustment& layer,
    const AdjustmentExecutionContext& context
);
} // namespace detail
} // namespace shadow::image
