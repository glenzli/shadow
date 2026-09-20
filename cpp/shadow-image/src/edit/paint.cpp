#include "working_color_math.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/paint.hpp>

namespace shadow::image::detail {
namespace {
bool unit(double v) {
    return std::isfinite(v) && v >= 0 && v <= 1;
}
[[noreturn]] void invalid(const char* message) {
    throw EditError(EditErrorCode::invalid_parameter, std::nullopt, message);
}
double decode(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
struct Dab {
    double x, y, pressure;
};
std::vector<Dab> dabs(const PaintStroke& stroke, const PaintLayerAdjustment& layer) {
    const double short_side = std::min(layer.coordinate_width, layer.coordinate_height);
    const double sx = layer.coordinate_width / short_side,
                 sy = layer.coordinate_height / short_side;
    const double spacing = stroke.radius * 0.25;
    std::vector<Dab> result{
        {stroke.points.front().x, stroke.points.front().y, stroke.points.front().pressure}
    };
    double carry = 0;
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        const auto& a = stroke.points[i - 1];
        const auto& b = stroke.points[i];
        const double length = std::hypot((b.x - a.x) * sx, (b.y - a.y) * sy);
        if (length <= 1e-12)
            continue;
        double distance = spacing - carry;
        for (; distance <= length; distance += spacing) {
            if (result.size() >= 32'768U)
                invalid("Paint stroke exceeds the dab budget");
            const double t = distance / length;
            result.push_back(
                {a.x + (b.x - a.x) * t,
                 a.y + (b.y - a.y) * t,
                 a.pressure + (b.pressure - a.pressure) * t}
            );
        }
        carry = length - (distance - spacing);
    }
    return result;
}
struct Rect {
    int l = 0, t = 0, r = 0, b = 0;
};
Rect bounds(
    const std::vector<Dab>& points,
    double rx,
    double ry,
    Dimensions full,
    const AdjustmentExecutionContext& context,
    Dimensions tile
) {
    double l = 1, t = 1, r = 0, b = 0;
    for (const auto& p : points) {
        l = std::min(l, p.x);
        t = std::min(t, p.y);
        r = std::max(r, p.x);
        b = std::max(b, p.y);
    }
    return {
        std::clamp(
            static_cast<int>(std::floor(l * full.width - rx)) - static_cast<int>(context.origin_x),
            0,
            static_cast<int>(tile.width)
        ),
        std::clamp(
            static_cast<int>(std::floor(t * full.height - ry)) - static_cast<int>(context.origin_y),
            0,
            static_cast<int>(tile.height)
        ),
        std::clamp(
            static_cast<int>(std::ceil(r * full.width + rx)) - static_cast<int>(context.origin_x),
            0,
            static_cast<int>(tile.width)
        ),
        std::clamp(
            static_cast<int>(std::ceil(b * full.height + ry)) - static_cast<int>(context.origin_y),
            0,
            static_cast<int>(tile.height)
        )
    };
}
} // namespace
void validate_paint_layer(const PaintLayerAdjustment& layer) {
    if (!layer.coordinate_width || !layer.coordinate_height || layer.coordinate_width > 131072
        || layer.coordinate_height > 131072 || layer.blend > 2 || !unit(layer.opacity)
        || layer.strokes.size() > 128)
        invalid("Invalid paint layer extent, blend or stroke budget");
    std::size_t count = 0;
    double total_dabs = 0;
    const double short_side = std::min(layer.coordinate_width, layer.coordinate_height);
    for (const auto& stroke : layer.strokes) {
        count += stroke.points.size();
        if (stroke.points.empty() || stroke.points.size() > 2048 || count > 32768
            || !unit(stroke.radius) || stroke.radius < 0.0001 || stroke.radius > 0.25
            || !unit(stroke.hardness) || !unit(stroke.flow) || !unit(stroke.opacity)
            || !std::all_of(stroke.color.begin(), stroke.color.end(), unit))
            invalid("Invalid paint stroke parameters");
        for (const auto& p : stroke.points)
            if (!unit(p.x) || !unit(p.y) || !unit(p.pressure))
                invalid("Invalid paint point");
        double distance = 0;
        for (std::size_t i = 1; i < stroke.points.size(); ++i)
            distance += std::hypot(
                (stroke.points[i].x - stroke.points[i - 1].x) * layer.coordinate_width / short_side,
                (stroke.points[i].y - stroke.points[i - 1].y) * layer.coordinate_height / short_side
            );
        const double dabs = 1 + distance / (stroke.radius * 0.25);
        total_dabs += dabs;
        if (dabs > 32760 || total_dabs > 262144)
            invalid("Paint dab budget exceeded");
    }
}
PreparedPaintOverlay prepare_paint_overlay(
    const PaintLayerAdjustment& layer,
    const FloatRgbImage& image,
    const AdjustmentExecutionContext& context
) {
    validate_paint_layer(layer);
    PreparedPaintOverlay result;
    if (layer.strokes.empty() || layer.opacity == 0)
        return result;
    const Dimensions full =
        context.full_dimensions.width ? context.full_dimensions : image.dimensions;
    const double short_side = std::min(layer.coordinate_width, layer.coordinate_height);
    std::vector<std::vector<Dab>> paths;
    std::vector<Rect> boxes;
    Rect region{
        static_cast<int>(image.dimensions.width),
        static_cast<int>(image.dimensions.height),
        0,
        0
    };
    for (const auto& stroke : layer.strokes) {
        paths.push_back(dabs(stroke, layer));
        const double rx = stroke.radius * short_side / layer.coordinate_width * full.width;
        const double ry = stroke.radius * short_side / layer.coordinate_height * full.height;
        auto box = bounds(paths.back(), rx, ry, full, context, image.dimensions);
        boxes.push_back(box);
        if (box.r > box.l && box.b > box.t) {
            region.l = std::min(region.l, box.l);
            region.t = std::min(region.t, box.t);
            region.r = std::max(region.r, box.r);
            region.b = std::max(region.b, box.b);
        }
    }
    if (region.r <= region.l || region.b <= region.t)
        return result;
    result.left = static_cast<std::uint32_t>(region.l);
    result.top = static_cast<std::uint32_t>(region.t);
    result.width = static_cast<std::uint32_t>(region.r - region.l);
    result.height = static_cast<std::uint32_t>(region.b - region.t);
    const std::size_t area = static_cast<std::size_t>(result.width) * result.height;
    if (area > 16U * 1024U * 1024U)
        invalid("Paint execution region exceeds 256 MiB; use tiled rendering");
    result.pixels.resize(area);
    std::vector<float> coverage(area);
    const auto transform = prepare_working_space_transform(image.working_space);
    constexpr Matrix3 srgb_to_xyz{
        {{0.4124564, 0.3575761, 0.1804375},
         {0.2126729, 0.7151522, 0.0721750},
         {0.0193339, 0.1191920, 0.9503041}}
    };
    for (std::size_t si = 0; si < layer.strokes.size(); ++si) {
        const auto& stroke = layer.strokes[si];
        const auto box = boxes[si];
        if (stroke.flow == 0 || stroke.opacity == 0 || box.r <= box.l || box.b <= box.t)
            continue;
        const double rx = stroke.radius * short_side / layer.coordinate_width * full.width;
        const double ry = stroke.radius * short_side / layer.coordinate_height * full.height;
        for (int y = box.t; y < box.b; ++y)
            std::fill_n(
                coverage.data() + static_cast<std::size_t>(y - region.t) * result.width
                    + static_cast<std::size_t>(box.l - region.l),
                box.r - box.l,
                0.0f
            );
        for (const auto& dab : paths[si]) {
            const double cx = dab.x * full.width - context.origin_x,
                         cy = dab.y * full.height - context.origin_y;
            const int l = std::max(box.l, static_cast<int>(std::floor(cx - rx))),
                      r = std::min(box.r, static_cast<int>(std::ceil(cx + rx)));
            const int t = std::max(box.t, static_cast<int>(std::floor(cy - ry))),
                      b = std::min(box.b, static_cast<int>(std::ceil(cy + ry)));
            for (int y = t; y < b; ++y)
                for (int x = l; x < r; ++x) {
                    const double d = std::hypot((x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry);
                    if (d >= 1)
                        continue;
                    double a = 1;
                    if (d > stroke.hardness) {
                        const double u = (d - stroke.hardness) / (1 - stroke.hardness);
                        a = 1 - u * u * (3 - 2 * u);
                    }
                    a *= stroke.flow * dab.pressure;
                    auto& c = coverage
                        [static_cast<std::size_t>(y - region.t) * result.width
                         + static_cast<std::size_t>(x - region.l)];
                    c = static_cast<float>(c + (1 - c) * a);
                }
        }
        const Vector3 rgb = apply_color_matrix(
            transform.xyz_to_rgb,
            apply_color_matrix(
                srgb_to_xyz,
                {decode(stroke.color[0]), decode(stroke.color[1]), decode(stroke.color[2])}
            )
        );
        for (int y = box.t; y < box.b; ++y)
            for (int x = box.l; x < box.r; ++x) {
                const auto i = static_cast<std::size_t>(y - region.t) * result.width
                               + static_cast<std::size_t>(x - region.l);
                const double a = coverage[i] * stroke.opacity;
                auto& p = result.pixels[i];
                for (std::size_t ch = 0; ch < 4; ++ch) {
                    const double ink = stroke.erase ? 0 : (ch == 3 ? 1 : rgb[ch]);
                    p[ch] = static_cast<float>(p[ch] * (1 - a) + ink * a);
                }
            }
    }
    return result;
}
void apply_paint_layer(
    FloatRgbImage& image,
    const PaintLayerAdjustment& layer,
    const AdjustmentExecutionContext& context
) {
    // Export may cover tens of megapixels. Rasterize bounded tiles while
    // retaining the same authored coordinates and destination image.
    constexpr std::uint32_t tile_extent = 1024;
    for (std::uint32_t top = 0; top < image.dimensions.height; top += tile_extent)
        for (std::uint32_t left = 0; left < image.dimensions.width; left += tile_extent) {
            FloatRgbImage tile;
            tile.dimensions = {
                std::min(tile_extent, image.dimensions.width - left),
                std::min(tile_extent, image.dimensions.height - top)
            };
            tile.working_space = image.working_space;
            auto tile_context = context;
            tile_context.origin_x += left;
            tile_context.origin_y += top;
            if (!tile_context.full_dimensions.width)
                tile_context.full_dimensions = image.dimensions;
            auto overlay = prepare_paint_overlay(layer, tile, tile_context);
            overlay.left += left;
            overlay.top += top;
            const auto transform = prepare_working_space_transform(image.working_space);
            const auto stride = image.row_stride_bytes / sizeof(float);
            for (std::uint32_t y = 0; y < overlay.height; ++y)
                for (std::uint32_t x = 0; x < overlay.width; ++x) {
                    const auto& p = overlay.pixels[static_cast<std::size_t>(y) * overlay.width + x];
                    if (p[3] <= 1e-8f)
                        continue;
                    const auto i = static_cast<std::size_t>(y + overlay.top) * stride
                                   + static_cast<std::size_t>(x + overlay.left) * 3;
                    const Vector3 base{
                        image.samples[i],
                        image.samples[i + 1],
                        image.samples[i + 2]
                    };
                    Vector3 ink{p[0] / p[3], p[1] / p[3], p[2] / p[3]};
                    const double a = p[3] * layer.opacity;
                    if (layer.blend) {
                        auto original = working_rgb_to_oklab(transform, base);
                        const auto paint = working_rgb_to_oklab(transform, ink);
                        if (layer.blend == 1) {
                            original[1] += (paint[1] - original[1]) * a;
                            original[2] += (paint[2] - original[2]) * a;
                        } else {
                            const double b = std::clamp(original[0], 0.0, 1.0);
                            const double light = std::pow(std::clamp(paint[0], 0.0, 1.0), 3);
                            const double c = light <= 0.0031308
                                                 ? light * 12.92
                                                 : 1.055 * std::pow(light, 1.0 / 2.4) - 0.055;
                            const double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
                            const double out = c <= 0.5 ? b - (1 - 2 * c) * b * (1 - b)
                                                        : b + (2 * c - 1) * (d - b);
                            original[0] += (out - b) * a;
                        }
                        ink = oklab_to_working_rgb(transform, original);
                        for (std::size_t ch = 0; ch < 3; ++ch)
                            image.samples[i + ch] = static_cast<float>(ink[ch]);
                    } else
                        for (std::size_t ch = 0; ch < 3; ++ch)
                            image.samples[i + ch] =
                                static_cast<float>(base[ch] + (ink[ch] - base[ch]) * a);
                }
        }
}
} // namespace shadow::image::detail
