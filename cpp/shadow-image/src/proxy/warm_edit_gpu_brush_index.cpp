#include "warm_edit_gpu_brush_index.hpp"

#include "../edit/local_mask_validation.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] std::size_t word_offset(const std::size_t byte_offset) noexcept {
    return byte_offset / sizeof(std::uint32_t);
}

[[nodiscard]] std::uint32_t grid_side(const double normalized_radius) noexcept {
    const double minimum_cell = 1.0 / static_cast<double>(maximum_warm_brush_grid_side);
    const double cell = std::max(normalized_radius, minimum_cell);
    return std::clamp(
        static_cast<std::uint32_t>(std::ceil(1.0 / cell)),
        1U,
        maximum_warm_brush_grid_side
    );
}

[[nodiscard]] std::uint32_t
cell_for(const double coordinate, const std::uint32_t cell_count) noexcept {
    const double scaled = std::clamp(coordinate, 0.0, 1.0) * static_cast<double>(cell_count);
    return std::min(static_cast<std::uint32_t>(scaled), cell_count - 1U);
}

struct BrushPoint final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] double
cross(const BrushPoint origin, const BrushPoint first, const BrushPoint second) noexcept {
    return (first.x - origin.x) * (second.y - origin.y)
           - (first.y - origin.y) * (second.x - origin.x);
}

[[nodiscard]] bool segments_intersect(
    const BrushPoint first_start,
    const BrushPoint first_end,
    const BrushPoint second_start,
    const BrushPoint second_end
) noexcept {
    const bool bounds_overlap =
        std::max(first_start.x, first_end.x) >= std::min(second_start.x, second_end.x)
        && std::max(second_start.x, second_end.x) >= std::min(first_start.x, first_end.x)
        && std::max(first_start.y, first_end.y) >= std::min(second_start.y, second_end.y)
        && std::max(second_start.y, second_end.y) >= std::min(first_start.y, first_end.y);
    if (!bounds_overlap) {
        return false;
    }
    const double first_side_start = cross(first_start, first_end, second_start);
    const double first_side_end = cross(first_start, first_end, second_end);
    const double second_side_start = cross(second_start, second_end, first_start);
    const double second_side_end = cross(second_start, second_end, first_end);
    return ((first_side_start <= 0.0 && first_side_end >= 0.0)
            || (first_side_start >= 0.0 && first_side_end <= 0.0))
           && ((second_side_start <= 0.0 && second_side_end >= 0.0)
               || (second_side_start >= 0.0 && second_side_end <= 0.0));
}

[[nodiscard]] double point_segment_distance_squared(
    const BrushPoint point,
    const BrushPoint start,
    const BrushPoint end
) noexcept {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double denominator = std::fma(dx, dx, dy * dy);
    if (denominator <= std::numeric_limits<double>::epsilon()) {
        const double point_dx = point.x - end.x;
        const double point_dy = point.y - end.y;
        return std::fma(point_dx, point_dx, point_dy * point_dy);
    }
    const double projection =
        std::clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) / denominator, 0.0, 1.0);
    const double nearest_x = std::fma(projection, dx, start.x);
    const double nearest_y = std::fma(projection, dy, start.y);
    const double point_dx = point.x - nearest_x;
    const double point_dy = point.y - nearest_y;
    return std::fma(point_dx, point_dx, point_dy * point_dy);
}

[[nodiscard]] double segment_distance_squared(
    const BrushPoint first_start,
    const BrushPoint first_end,
    const BrushPoint second_start,
    const BrushPoint second_end
) noexcept {
    if (segments_intersect(first_start, first_end, second_start, second_end)) {
        return 0.0;
    }
    return std::min({
        point_segment_distance_squared(first_start, second_start, second_end),
        point_segment_distance_squared(first_end, second_start, second_end),
        point_segment_distance_squared(second_start, first_start, first_end),
        point_segment_distance_squared(second_end, first_start, first_end),
    });
}

[[nodiscard]] double point_rectangle_distance_squared(
    const BrushPoint point,
    const BrushPoint minimum,
    const BrushPoint maximum
) noexcept {
    const double dx = std::max({minimum.x - point.x, 0.0, point.x - maximum.x});
    const double dy = std::max({minimum.y - point.y, 0.0, point.y - maximum.y});
    return std::fma(dx, dx, dy * dy);
}

[[nodiscard]] bool capsule_intersects_cell(
    const WarmBrushCapsule capsule,
    const std::uint32_t column,
    const std::uint32_t row,
    const std::uint32_t grid_columns,
    const std::uint32_t grid_rows,
    const double scale_x,
    const double scale_y,
    const double radius
) noexcept {
    const BrushPoint start{
        .x = static_cast<double>(capsule.x0) * scale_x,
        .y = static_cast<double>(capsule.y0) * scale_y,
    };
    const BrushPoint end{
        .x = static_cast<double>(capsule.x1) * scale_x,
        .y = static_cast<double>(capsule.y1) * scale_y,
    };
    const BrushPoint minimum{
        .x = static_cast<double>(column) * scale_x / grid_columns,
        .y = static_cast<double>(row) * scale_y / grid_rows,
    };
    const BrushPoint maximum{
        .x = static_cast<double>(column + 1U) * scale_x / grid_columns,
        .y = static_cast<double>(row + 1U) * scale_y / grid_rows,
    };
    double distance_squared = std::min(
        point_rectangle_distance_squared(start, minimum, maximum),
        point_rectangle_distance_squared(end, minimum, maximum)
    );
    const std::array edges{
        std::pair{BrushPoint{minimum.x, minimum.y}, BrushPoint{maximum.x, minimum.y}},
        std::pair{BrushPoint{maximum.x, minimum.y}, BrushPoint{maximum.x, maximum.y}},
        std::pair{BrushPoint{maximum.x, maximum.y}, BrushPoint{minimum.x, maximum.y}},
        std::pair{BrushPoint{minimum.x, maximum.y}, BrushPoint{minimum.x, minimum.y}},
    };
    for (const auto& [edge_start, edge_end] : edges) {
        distance_squared =
            std::min(distance_squared, segment_distance_squared(start, end, edge_start, edge_end));
    }
    return distance_squared <= std::fma(radius, radius, 1.0e-14);
}

[[nodiscard]] std::vector<WarmBrushCapsule> brush_capsules(const LocalMask& mask) {
    std::vector<WarmBrushCapsule> result;
    result.reserve(mask.points.size());
    for (std::size_t index = 0U; index < mask.points.size(); ++index) {
        const LocalMaskPoint& point = mask.points[index];
        const bool has_incoming = index > 0U && !point.begins_stroke;
        const bool has_outgoing =
            index + 1U < mask.points.size() && !mask.points[index + 1U].begins_stroke;
        if (has_incoming) {
            const LocalMaskPoint& start = mask.points[index - 1U];
            result.push_back(
                WarmBrushCapsule{
                    .x0 = static_cast<float>(start.x),
                    .y0 = static_cast<float>(start.y),
                    .x1 = static_cast<float>(point.x),
                    .y1 = static_cast<float>(point.y),
                }
            );
        } else if (!has_outgoing) {
            // A connected segment already contains both endpoint disks. Only a one-point stroke
            // needs its own zero-length capsule.
            result.push_back(
                WarmBrushCapsule{
                    .x0 = static_cast<float>(point.x),
                    .y0 = static_cast<float>(point.y),
                    .x1 = static_cast<float>(point.x),
                    .y1 = static_cast<float>(point.y),
                }
            );
        }
    }
    return result;
}

void append_float_word(std::vector<std::uint32_t>& words, const float value) {
    words.push_back(std::bit_cast<std::uint32_t>(value));
}

} // namespace

bool WarmGpuBrushIndex::valid() const noexcept {
    const std::uint64_t cell_count = static_cast<std::uint64_t>(grid_columns) * grid_rows;
    if (grid_columns == 0U || grid_rows == 0U || grid_columns > maximum_warm_brush_grid_side
        || grid_rows > maximum_warm_brush_grid_side || capsule_count == 0U
        || reference_count > maximum_warm_brush_references || capsule_offset_bytes != 0U
        || cell_range_offset_bytes
               != static_cast<std::size_t>(capsule_count) * sizeof(WarmBrushCapsule)
        || reference_offset_bytes
               != cell_range_offset_bytes
                      + static_cast<std::size_t>(cell_count) * sizeof(WarmBrushCellRange)
        || words.size() * sizeof(std::uint32_t)
               != reference_offset_bytes
                      + static_cast<std::size_t>(reference_count) * sizeof(std::uint32_t)) {
        return false;
    }
    const std::size_t range_word = word_offset(cell_range_offset_bytes);
    const std::size_t reference_word = word_offset(reference_offset_bytes);
    for (std::uint64_t cell_index = 0U; cell_index < cell_count; ++cell_index) {
        const std::size_t offset = range_word + static_cast<std::size_t>(cell_index) * 2U;
        const std::uint32_t begin = words[offset];
        const std::uint32_t count = words[offset + 1U];
        if (begin > reference_count || count > reference_count - begin) {
            return false;
        }
    }
    for (std::uint32_t index = 0U; index < reference_count; ++index) {
        if (words[reference_word + index] >= capsule_count) {
            return false;
        }
    }
    return true;
}

WarmBrushCapsule WarmGpuBrushIndex::capsule(const std::uint32_t index) const {
    const std::size_t offset =
        word_offset(capsule_offset_bytes) + static_cast<std::size_t>(index) * 4U;
    return WarmBrushCapsule{
        .x0 = std::bit_cast<float>(words.at(offset)),
        .y0 = std::bit_cast<float>(words.at(offset + 1U)),
        .x1 = std::bit_cast<float>(words.at(offset + 2U)),
        .y1 = std::bit_cast<float>(words.at(offset + 3U)),
    };
}

WarmBrushCellRange WarmGpuBrushIndex::cell(const std::uint32_t index) const {
    const std::size_t offset =
        word_offset(cell_range_offset_bytes) + static_cast<std::size_t>(index) * 2U;
    return WarmBrushCellRange{
        .offset = words.at(offset),
        .count = words.at(offset + 1U),
    };
}

std::uint32_t WarmGpuBrushIndex::reference(const std::uint32_t index) const {
    return words.at(word_offset(reference_offset_bytes) + index);
}

WarmGpuBrushIndexPreparation
prepare_warm_gpu_brush_index(const LocalMask& mask, const Dimensions full_dimensions) {
    validate_local_mask(mask);
    if (mask.kind != LocalMaskKind::brush) {
        return WarmGpuBrushIndexPreparation{
            .diagnostic = "warm brush index requires a brush mask",
        };
    }
    if (full_dimensions.width == 0U || full_dimensions.height == 0U) {
        return WarmGpuBrushIndexPreparation{
            .diagnostic = "warm brush index requires non-empty full-image dimensions",
        };
    }
    if (mask.points.empty()) {
        return WarmGpuBrushIndexPreparation{
            .diagnostic = "warm brush index requires at least one brush point",
        };
    }

    const double shorter_side =
        static_cast<double>(std::min(full_dimensions.width, full_dimensions.height));
    const double scale_x = static_cast<double>(full_dimensions.width) / shorter_side;
    const double scale_y = static_cast<double>(full_dimensions.height) / shorter_side;
    const std::uint32_t grid_columns = grid_side(mask.radius_x / scale_x);
    const std::uint32_t grid_rows = grid_side(mask.radius_x / scale_y);
    const std::uint32_t cell_count = grid_columns * grid_rows;
    const std::vector<WarmBrushCapsule> capsules = brush_capsules(mask);
    if (capsules.empty() || capsules.size() > std::numeric_limits<std::uint32_t>::max()) {
        return WarmGpuBrushIndexPreparation{
            .diagnostic = "warm brush index produced an invalid capsule count",
        };
    }

    std::vector<std::vector<std::uint32_t>> cell_references(cell_count);
    const double radius_x = mask.radius_x / scale_x;
    const double radius_y = mask.radius_x / scale_y;
    std::uint64_t reference_count = 0U;
    for (std::size_t capsule_index = 0U; capsule_index < capsules.size(); ++capsule_index) {
        const WarmBrushCapsule& capsule = capsules[capsule_index];
        const std::uint32_t minimum_column =
            cell_for(std::min(capsule.x0, capsule.x1) - radius_x, grid_columns);
        const std::uint32_t maximum_column =
            cell_for(std::max(capsule.x0, capsule.x1) + radius_x, grid_columns);
        const std::uint32_t minimum_row =
            cell_for(std::min(capsule.y0, capsule.y1) - radius_y, grid_rows);
        const std::uint32_t maximum_row =
            cell_for(std::max(capsule.y0, capsule.y1) + radius_y, grid_rows);
        for (std::uint32_t row = minimum_row; row <= maximum_row; ++row) {
            for (std::uint32_t column = minimum_column; column <= maximum_column; ++column) {
                if (!capsule_intersects_cell(
                        capsule,
                        column,
                        row,
                        grid_columns,
                        grid_rows,
                        scale_x,
                        scale_y,
                        mask.radius_x
                    )) {
                    continue;
                }
                if (reference_count == maximum_warm_brush_references) {
                    return WarmGpuBrushIndexPreparation{
                        .diagnostic =
                            "warm brush spatial index exceeds its bounded reference budget",
                    };
                }
                cell_references[static_cast<std::size_t>(row) * grid_columns + column].push_back(
                    static_cast<std::uint32_t>(capsule_index)
                );
                ++reference_count;
            }
        }
    }

    WarmGpuBrushIndex result{
        .grid_columns = grid_columns,
        .grid_rows = grid_rows,
        .capsule_count = static_cast<std::uint32_t>(capsules.size()),
        .reference_count = static_cast<std::uint32_t>(reference_count),
        .capsule_offset_bytes = 0U,
        .cell_range_offset_bytes = capsules.size() * sizeof(WarmBrushCapsule),
        .reference_offset_bytes =
            capsules.size() * sizeof(WarmBrushCapsule)
            + static_cast<std::size_t>(cell_count) * sizeof(WarmBrushCellRange),
    };
    result.words.reserve(
        result.reference_offset_bytes / sizeof(std::uint32_t) + result.reference_count
    );
    for (const WarmBrushCapsule& capsule : capsules) {
        append_float_word(result.words, capsule.x0);
        append_float_word(result.words, capsule.y0);
        append_float_word(result.words, capsule.x1);
        append_float_word(result.words, capsule.y1);
    }
    std::uint32_t running_offset = 0U;
    for (const auto& references : cell_references) {
        result.words.push_back(running_offset);
        result.words.push_back(static_cast<std::uint32_t>(references.size()));
        running_offset += static_cast<std::uint32_t>(references.size());
    }
    for (const auto& references : cell_references) {
        result.words.insert(result.words.end(), references.begin(), references.end());
    }
    if (!result.valid()) {
        return WarmGpuBrushIndexPreparation{
            .diagnostic = "warm brush spatial index failed its internal layout validation",
        };
    }
    return WarmGpuBrushIndexPreparation{.index = std::move(result)};
}

} // namespace shadow::image::detail
