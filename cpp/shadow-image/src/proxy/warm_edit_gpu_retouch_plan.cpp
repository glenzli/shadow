#include "warm_edit_gpu_retouch_plan.hpp"

#include <shadow/image/retouch.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

constexpr std::uint32_t maximum_grid_extent = 32U;
constexpr std::size_t maximum_reference_count = 4U * 1'024U * 1'024U;

struct RasterPoint final {
    double x = 0.0;
    double y = 0.0;
};

struct SourceOffset final {
    double x = 0.0;
    double y = 0.0;
};

struct RegionGeometry final {
    std::vector<RasterPoint> points;
    double radius_x = 0.0;
    double radius_y = 0.0;
    SourceOffset donor_offset;
    double feather = 0.0;
};

struct IntegerBounds final {
    std::int64_t lower_x = 0;
    std::int64_t lower_y = 0;
    std::int64_t upper_x = -1;
    std::int64_t upper_y = -1;
};

[[nodiscard]] Dimensions execution_full_dimensions(
    const Dimensions dimensions,
    const AdjustmentExecutionContext context
) noexcept {
    return context.full_dimensions.width == 0U || context.full_dimensions.height == 0U
               ? dimensions
               : context.full_dimensions;
}

[[nodiscard]] RasterPoint raster_point(
    const RetouchStrokePoint point,
    const Dimensions full,
    const AdjustmentExecutionContext context
) noexcept {
    return {
        .x =
            point.x * static_cast<double>(full.width) - static_cast<double>(context.origin_x) - 0.5,
        .y = point.y * static_cast<double>(full.height) - static_cast<double>(context.origin_y)
             - 0.5,
    };
}

[[nodiscard]] SourceOffset authored_or_automatic_offset(
    const double authored_x_radii,
    const double authored_y_radii,
    const double radius_x,
    const double radius_y,
    const double normalized_center_x,
    const double normalized_center_y
) noexcept {
    constexpr double authored_zero_epsilon = 1.0e-9;
    if (std::abs(authored_x_radii) > authored_zero_epsilon
        || std::abs(authored_y_radii) > authored_zero_epsilon) {
        return {
            .x = authored_x_radii * radius_x,
            .y = authored_y_radii * radius_y,
        };
    }
    return {
        .x = (normalized_center_x <= 0.5 ? 3.0 : -3.0) * radius_x,
        .y = (normalized_center_y <= 0.5 ? 1.5 : -1.5) * radius_y,
    };
}

[[nodiscard]] std::optional<RegionGeometry> target_geometry(
    const SpotHealTarget& target,
    const Dimensions full,
    const AdjustmentExecutionContext context,
    const double scale_x,
    const double scale_y
) {
    if (target.mode != SpotRepairMode::clone) {
        return std::nullopt;
    }
    const double radius_x = static_cast<double>(target.radius_level_zero_pixels) * scale_x;
    const double radius_y = static_cast<double>(target.radius_level_zero_pixels) * scale_y;
    const RasterPoint center =
        raster_point(RetouchStrokePoint{.x = target.center_x, .y = target.center_y}, full, context);
    return RegionGeometry{
        .points = {center},
        .radius_x = radius_x,
        .radius_y = radius_y,
        .donor_offset = authored_or_automatic_offset(
            target.source_offset_x_radii,
            target.source_offset_y_radii,
            radius_x,
            radius_y,
            target.center_x,
            target.center_y
        ),
        .feather = target.feather,
    };
}

[[nodiscard]] std::optional<RegionGeometry> stroke_geometry(
    const RetouchStroke& stroke,
    const Dimensions full,
    const AdjustmentExecutionContext context,
    const double scale_x,
    const double scale_y
) {
    if (stroke.mode != SpotRepairMode::clone) {
        return std::nullopt;
    }
    RegionGeometry result{
        .radius_x = static_cast<double>(stroke.radius_level_zero_pixels) * scale_x,
        .radius_y = static_cast<double>(stroke.radius_level_zero_pixels) * scale_y,
        .feather = stroke.feather,
    };
    result.points.reserve(stroke.points.size());
    double lower_x = 1.0;
    double upper_x = 0.0;
    double lower_y = 1.0;
    double upper_y = 0.0;
    for (const RetouchStrokePoint point : stroke.points) {
        result.points.push_back(raster_point(point, full, context));
        lower_x = std::min(lower_x, point.x);
        upper_x = std::max(upper_x, point.x);
        lower_y = std::min(lower_y, point.y);
        upper_y = std::max(upper_y, point.y);
    }
    const double center_x = std::midpoint(lower_x, upper_x);
    const double center_y = std::midpoint(lower_y, upper_y);
    const bool automatic =
        stroke.source_offset_x_radii == 0.0 && stroke.source_offset_y_radii == 0.0;
    result.donor_offset = automatic
        ? (upper_x - lower_x >= upper_y - lower_y
            ? SourceOffset{
                .x = 0.0,
                .y = (center_y <= 0.5 ? 3.0 : -3.0) * result.radius_y,
            }
            : SourceOffset{
                .x = (center_x <= 0.5 ? 3.0 : -3.0) * result.radius_x,
                .y = 0.0,
            })
        : authored_or_automatic_offset(
            stroke.source_offset_x_radii,
            stroke.source_offset_y_radii,
            result.radius_x,
            result.radius_y,
            center_x,
            center_y
        );
    return result;
}

[[nodiscard]] IntegerBounds
region_bounds(const RegionGeometry& geometry, const Dimensions dimensions) noexcept {
    double lower_x = std::numeric_limits<double>::infinity();
    double upper_x = -std::numeric_limits<double>::infinity();
    double lower_y = std::numeric_limits<double>::infinity();
    double upper_y = -std::numeric_limits<double>::infinity();
    for (const RasterPoint point : geometry.points) {
        lower_x = std::min(lower_x, point.x - geometry.radius_x);
        upper_x = std::max(upper_x, point.x + geometry.radius_x);
        lower_y = std::min(lower_y, point.y - geometry.radius_y);
        upper_y = std::max(upper_y, point.y + geometry.radius_y);
    }
    const std::int64_t maximum_x = static_cast<std::int64_t>(dimensions.width) - 1;
    const std::int64_t maximum_y = static_cast<std::int64_t>(dimensions.height) - 1;
    return {
        .lower_x = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(lower_x)) - 1),
        .lower_y = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(lower_y)) - 1),
        .upper_x =
            std::min<std::int64_t>(maximum_x, static_cast<std::int64_t>(std::ceil(upper_x)) + 1),
        .upper_y =
            std::min<std::int64_t>(maximum_y, static_cast<std::int64_t>(std::ceil(upper_y)) + 1),
    };
}

[[nodiscard]] std::vector<WarmRetouchCapsule> capsules(const RegionGeometry& geometry) {
    const std::size_t count = geometry.points.size() == 1U ? 1U : geometry.points.size() - 1U;
    std::vector<WarmRetouchCapsule> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const RasterPoint first = geometry.points[index];
        const RasterPoint last = geometry.points[geometry.points.size() == 1U ? 0U : index + 1U];
        result.push_back(
            WarmRetouchCapsule{
                .x0 = static_cast<float>(first.x),
                .y0 = static_cast<float>(first.y),
                .x1 = static_cast<float>(last.x),
                .y1 = static_cast<float>(last.y),
            }
        );
    }
    return result;
}

[[nodiscard]] bool capsule_may_cover_cell(
    const WarmRetouchCapsule capsule,
    const double radius_x,
    const double radius_y,
    const double lower_x,
    const double lower_y,
    const double upper_x,
    const double upper_y
) noexcept {
    return static_cast<double>(std::min(capsule.x0, capsule.x1)) - radius_x <= upper_x
           && static_cast<double>(std::max(capsule.x0, capsule.x1)) + radius_x >= lower_x
           && static_cast<double>(std::min(capsule.y0, capsule.y1)) - radius_y <= upper_y
           && static_cast<double>(std::max(capsule.y0, capsule.y1)) + radius_y >= lower_y;
}

template <typename Record>
[[nodiscard]] std::size_t
append_records(std::vector<WarmRetouchWord>& words, const std::span<const Record> records) {
    static_assert(sizeof(Record) % sizeof(std::uint32_t) == 0U);
    const std::size_t offset = words.size() * sizeof(std::uint32_t);
    const std::size_t old_size = words.size();
    words.resize(old_size + records.size_bytes() / sizeof(std::uint32_t));
    if (!records.empty()) {
        std::memcpy(
            words.data() + static_cast<std::ptrdiff_t>(old_size),
            records.data(),
            records.size_bytes()
        );
    }
    return offset;
}

[[nodiscard]] bool append_region(
    WarmRetouchCloneStage& stage,
    const RegionGeometry& geometry,
    const Dimensions dimensions
) {
    const IntegerBounds bounds = region_bounds(geometry, dimensions);
    if (bounds.lower_x > bounds.upper_x || bounds.lower_y > bounds.upper_y) {
        return true;
    }
    const auto width = static_cast<std::uint32_t>(bounds.upper_x - bounds.lower_x + 1);
    const auto height = static_cast<std::uint32_t>(bounds.upper_y - bounds.lower_y + 1);
    const std::uint32_t columns = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(
            std::ceil(static_cast<double>(width) / std::max(8.0, geometry.radius_x * 2.0))
        ),
        1U,
        maximum_grid_extent
    );
    const std::uint32_t rows = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(
            std::ceil(static_cast<double>(height) / std::max(8.0, geometry.radius_y * 2.0))
        ),
        1U,
        maximum_grid_extent
    );
    const std::vector<WarmRetouchCapsule> region_capsules = capsules(geometry);
    std::vector<WarmRetouchCellRange> cells(static_cast<std::size_t>(columns) * rows);
    std::vector<std::uint32_t> references;
    for (std::uint32_t row = 0U; row < rows; ++row) {
        const double cell_lower_y =
            static_cast<double>(bounds.lower_y)
            + static_cast<double>(height) * static_cast<double>(row) / static_cast<double>(rows);
        const double cell_upper_y = static_cast<double>(bounds.lower_y)
                                    + static_cast<double>(height) * static_cast<double>(row + 1U)
                                          / static_cast<double>(rows);
        for (std::uint32_t column = 0U; column < columns; ++column) {
            const double cell_lower_x = static_cast<double>(bounds.lower_x)
                                        + static_cast<double>(width) * static_cast<double>(column)
                                              / static_cast<double>(columns);
            const double cell_upper_x = static_cast<double>(bounds.lower_x)
                                        + static_cast<double>(width)
                                              * static_cast<double>(column + 1U)
                                              / static_cast<double>(columns);
            WarmRetouchCellRange& range = cells[static_cast<std::size_t>(row) * columns + column];
            range.offset = static_cast<std::uint32_t>(references.size());
            for (std::size_t index = 0U; index < region_capsules.size(); ++index) {
                if (capsule_may_cover_cell(
                        region_capsules[index],
                        geometry.radius_x,
                        geometry.radius_y,
                        cell_lower_x,
                        cell_lower_y,
                        cell_upper_x,
                        cell_upper_y
                    )) {
                    if (references.size() >= maximum_reference_count) {
                        return false;
                    }
                    references.push_back(static_cast<std::uint32_t>(index));
                    ++range.count;
                }
            }
        }
    }

    WarmRetouchCloneRegion region{
        .parameters = WarmRetouchCloneParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .input_row_floats = dimensions.width * 3U,
            .bounds_origin_x = static_cast<std::uint32_t>(bounds.lower_x),
            .bounds_origin_y = static_cast<std::uint32_t>(bounds.lower_y),
            .bounds_width = width,
            .bounds_height = height,
            .grid_columns = columns,
            .grid_rows = rows,
            .capsule_count = static_cast<std::uint32_t>(region_capsules.size()),
            .reference_count = static_cast<std::uint32_t>(references.size()),
            .radius_x = static_cast<float>(geometry.radius_x),
            .radius_y = static_cast<float>(geometry.radius_y),
            .donor_offset_x = static_cast<float>(geometry.donor_offset.x),
            .donor_offset_y = static_cast<float>(geometry.donor_offset.y),
            .feather = static_cast<float>(geometry.feather),
        },
    };
    region.capsule_offset_bytes =
        append_records(stage.packed_geometry, std::span<const WarmRetouchCapsule>(region_capsules));
    region.cell_offset_bytes =
        append_records(stage.packed_geometry, std::span<const WarmRetouchCellRange>(cells));
    region.reference_offset_bytes =
        append_records(stage.packed_geometry, std::span<const std::uint32_t>(references));
    stage.regions.push_back(region);
    return true;
}

} // namespace

bool WarmRetouchCloneStage::valid() const noexcept {
    if (regions.empty() || packed_geometry.empty()) {
        return false;
    }
    const std::size_t bytes = packed_geometry.size() * sizeof(std::uint32_t);
    return std::ranges::all_of(regions, [bytes](const WarmRetouchCloneRegion& region) {
        const auto& parameters = region.parameters;
        const std::size_t capsule_bytes =
            static_cast<std::size_t>(parameters.capsule_count) * sizeof(WarmRetouchCapsule);
        const std::size_t cell_bytes = static_cast<std::size_t>(parameters.grid_columns)
                                       * parameters.grid_rows * sizeof(WarmRetouchCellRange);
        const std::size_t reference_bytes =
            static_cast<std::size_t>(parameters.reference_count) * sizeof(std::uint32_t);
        return parameters.width > 0U && parameters.height > 0U
               && parameters.input_row_floats == parameters.width * 3U
               && parameters.bounds_width > 0U && parameters.bounds_height > 0U
               && parameters.bounds_origin_x <= parameters.width
               && parameters.bounds_width <= parameters.width - parameters.bounds_origin_x
               && parameters.bounds_origin_y <= parameters.height
               && parameters.bounds_height <= parameters.height - parameters.bounds_origin_y
               && parameters.grid_columns > 0U && parameters.grid_rows > 0U
               && parameters.capsule_count > 0U && std::isfinite(parameters.radius_x)
               && parameters.radius_x > 0.0F && std::isfinite(parameters.radius_y)
               && parameters.radius_y > 0.0F && std::isfinite(parameters.donor_offset_x)
               && std::isfinite(parameters.donor_offset_y) && std::isfinite(parameters.feather)
               && region.capsule_offset_bytes <= bytes
               && capsule_bytes <= bytes - region.capsule_offset_bytes
               && region.cell_offset_bytes <= bytes
               && cell_bytes <= bytes - region.cell_offset_bytes
               && region.reference_offset_bytes <= bytes
               && reference_bytes <= bytes - region.reference_offset_bytes;
    });
}

std::optional<WarmRetouchCloneStage> prepare_warm_retouch_clone_stage(
    const SpotHealAdjustment& adjustment,
    const Dimensions dimensions,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y,
    const AdjustmentExecutionContext context
) {
    validate_spot_heal(adjustment);
    const Dimensions full = execution_full_dimensions(dimensions, context);
    if (dimensions.width == 0U || dimensions.height == 0U || full.width == 0U || full.height == 0U
        || context.origin_x > full.width || context.origin_y > full.height
        || dimensions.width > full.width - context.origin_x
        || dimensions.height > full.height - context.origin_y
        || !std::isfinite(level_zero_to_raster_scale_x) || level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(level_zero_to_raster_scale_y) || level_zero_to_raster_scale_y <= 0.0) {
        return std::nullopt;
    }
    WarmRetouchCloneStage result;
    for (const SpotHealTarget& target : adjustment.spots) {
        const auto geometry = target_geometry(
            target,
            full,
            context,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        if (!geometry.has_value()) {
            return std::nullopt;
        }
        if (!append_region(result, *geometry, dimensions)) {
            return std::nullopt;
        }
    }
    for (const RetouchStroke& stroke : adjustment.strokes) {
        const auto geometry = stroke_geometry(
            stroke,
            full,
            context,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        if (!geometry.has_value()) {
            return std::nullopt;
        }
        if (!append_region(result, *geometry, dimensions)) {
            return std::nullopt;
        }
    }
    return result.valid() ? std::optional<WarmRetouchCloneStage>{std::move(result)} : std::nullopt;
}

} // namespace shadow::image::detail
