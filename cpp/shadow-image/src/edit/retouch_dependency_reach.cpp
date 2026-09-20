#include "retouch_dependency_reach.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <type_traits>
#include <vector>

namespace shadow::image::detail {
namespace {
using Bounds = RetouchRasterBounds;
Bounds grow(Bounds a, double x, double y) {
    return {a.lower_x - x, a.upper_x + x, a.lower_y - y, a.upper_y + y};
}
void unite(Bounds& a, Bounds b) {
    a.lower_x = std::min(a.lower_x, b.lower_x);
    a.upper_x = std::max(a.upper_x, b.upper_x);
    a.lower_y = std::min(a.lower_y, b.lower_y);
    a.upper_y = std::max(a.upper_y, b.upper_y);
}
Bounds mapped(Bounds a, const RetouchSourceMapping& map) {
    Bounds result{
        map.source_x(a.lower_x, a.lower_y),
        map.source_x(a.lower_x, a.lower_y),
        map.source_y(a.lower_x, a.lower_y),
        map.source_y(a.lower_x, a.lower_y)
    };
    for (double x : {a.lower_x, a.upper_x})
        for (double y : {a.lower_y, a.upper_y}) {
            const auto px = map.source_x(x, y), py = map.source_y(x, y);
            unite(result, {px, px, py, py});
        }
    return result;
}
struct Region {
    Bounds target;
    std::optional<RetouchSourceMapping> mapping;
    RetouchSourceReach reach;
    double halo_x = 0, halo_y = 0;
    bool frequency = false;
};
} // namespace
RetouchSourceReach retouch_dependency_reach(
    const SpotHealAdjustment& adjustment,
    double scale_x,
    double scale_y,
    Dimensions dimensions
) {
    std::vector<Region> regions;
    RetouchSourceReach sum;
    const auto append = [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        SpotHealAdjustment single;
        Bounds normalized;
        if constexpr (std::is_same_v<Value, SpotHealTarget>) {
            single.spots = {value};
            normalized = {value.center_x, value.center_x, value.center_y, value.center_y};
        } else {
            single.strokes = {value};
            normalized = {1, 0, 1, 0};
            for (const auto point : value.points)
                unite(normalized, {point.x, point.x, point.y, point.y});
        }
        const auto reach = retouch_source_reach(single, scale_x, scale_y, dimensions);
        sum.horizontal += reach.horizontal;
        sum.vertical += reach.vertical;
        const double rx = value.radius_level_zero_pixels * scale_x;
        const double ry = value.radius_level_zero_pixels * scale_y;
        const double cx = std::midpoint(normalized.lower_x, normalized.upper_x);
        const double cy = std::midpoint(normalized.lower_y, normalized.upper_y);
        Bounds target{
            normalized.lower_x * dimensions.width - 0.5 - rx,
            normalized.upper_x * dimensions.width - 0.5 + rx,
            normalized.lower_y * dimensions.height - 0.5 - ry,
            normalized.upper_y * dimensions.height - 0.5 + ry
        };
        double dx = value.source_offset_x_radii, dy = value.source_offset_y_radii;
        if (dx == 0 && dy == 0) {
            const double distance =
                std::min(3.0, maximum_retouch_source_offset_radii(value.radius_level_zero_pixels));
            if constexpr (std::is_same_v<Value, SpotHealTarget>) {
                dx = cx <= 0.5 ? distance : -distance;
                dy = cy <= 0.5 ? distance * 0.5 : -distance * 0.5;
            } else if (
                normalized.upper_x - normalized.lower_x >= normalized.upper_y - normalized.lower_y
            )
                dy = cy <= 0.5 ? distance : -distance;
            else
                dx = cx <= 0.5 ? distance : -distance;
        }
        const auto mapping = dimensions.width && dimensions.height
                                 ? clamp_retouch_source_mapping_to_image(
                                       make_retouch_source_mapping(
                                           value.source_rotation_degrees,
                                           value.source_scale,
                                           value.source_flip_horizontal,
                                           value.source_flip_vertical,
                                           scale_x,
                                           scale_y,
                                           cx * dimensions.width - 0.5,
                                           cy * dimensions.height - 0.5,
                                           dx * rx,
                                           dy * ry
                                       ),
                                       target,
                                       dimensions
                                   )
                                 : std::nullopt;
        const bool frequency =
            value.mode == SpotRepairMode::tone || value.mode == SpotRepairMode::texture;
        regions.push_back(
            {target,
             mapping,
             reach,
             frequency ? std::max(1.0, std::ceil(3 * value.frequency_radius * scale_x)) + 1 : 0,
             frequency ? std::max(1.0, std::ceil(3 * value.frequency_radius * scale_y)) + 1 : 0,
             frequency}
        );
    };
    for (const auto& v : adjustment.spots)
        append(v);
    for (const auto& v : adjustment.strokes)
        append(v);
    if (!dimensions.width || !dimensions.height)
        return sum;
    RetouchSourceReach result, prefix;
    for (std::size_t last = 0; last < regions.size(); ++last) {
        prefix.horizontal += regions[last].reach.horizontal;
        prefix.vertical += regions[last].reach.vertical;
        const auto output = regions[last].target;
        Bounds required = output;
        for (std::size_t j = last + 1; j-- > 0;) {
            const auto& region = regions[j];
            Bounds intersection{
                std::max(required.lower_x, region.target.lower_x),
                std::min(required.upper_x, region.target.upper_x),
                std::max(required.lower_y, region.target.lower_y),
                std::min(required.upper_y, region.target.upper_y)
            };
            if (intersection.lower_x > intersection.upper_x
                || intersection.lower_y > intersection.upper_y)
                continue;
            if (region.frequency && region.mapping) {
                unite(required, grow(intersection, region.halo_x, region.halo_y));
                unite(
                    required,
                    grow(mapped(intersection, *region.mapping), region.halo_x, region.halo_y)
                );
            } else {
                unite(required, grow(intersection, region.reach.horizontal, region.reach.vertical));
            }
        }
        // Both bounds are conservative: the prefix sum works for any placement;
        // the spatial envelope accounts for every possible input of this region.
        const double horizontal =
            std::max(required.upper_x - output.lower_x, output.upper_x - required.lower_x);
        const double vertical =
            std::max(required.upper_y - output.lower_y, output.upper_y - required.lower_y);
        result.horizontal = std::max(result.horizontal, std::min(prefix.horizontal, horizontal));
        result.vertical = std::max(result.vertical, std::min(prefix.vertical, vertical));
    }
    return result;
}
} // namespace shadow::image::detail
