#include "../src/proxy/warm_edit_gpu_retouch_plan.hpp"

#include <shadow/image/adjustment_parameters.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

template <typename Record>
[[nodiscard]] std::vector<Record> records(
    const image::detail::WarmRetouchStage& stage,
    const std::size_t offset,
    const std::size_t count
) {
    std::vector<Record> result(count);
    std::memcpy(
        result.data(),
        reinterpret_cast<const std::byte*>(stage.packed_geometry.data())
            + static_cast<std::ptrdiff_t>(offset),
        count * sizeof(Record)
    );
    return result;
}

[[nodiscard]] double capsule_distance(
    const double x,
    const double y,
    const image::detail::WarmRetouchCapsule capsule,
    const double radius_x,
    const double radius_y
) {
    const double point_x = (x - capsule.x0) / radius_x;
    const double point_y = (y - capsule.y0) / radius_y;
    const double segment_x = (capsule.x1 - capsule.x0) / radius_x;
    const double segment_y = (capsule.y1 - capsule.y0) / radius_y;
    const double length_squared = segment_x * segment_x + segment_y * segment_y;
    const double projection =
        length_squared <= 1.0e-12
            ? 0.0
            : std::clamp((point_x * segment_x + point_y * segment_y) / length_squared, 0.0, 1.0);
    return std::hypot(point_x - projection * segment_x, point_y - projection * segment_y);
}

void continuous_clone_geometry_has_complete_bounded_candidates() {
    const image::SpotHealAdjustment adjustment{
        .strokes = {{
            .points =
                {
                    {.x = 0.25, .y = 0.28},
                    {.x = 0.40, .y = 0.52},
                    {.x = 0.55, .y = 0.70},
                },
            .radius_level_zero_pixels = 8U,
            .mode = image::SpotRepairMode::clone,
            .source_offset_x_radii = 1.5,
            .source_offset_y_radii = -0.5,
            .source_rotation_degrees = 90.0,
            .source_scale = 1.5,
            .source_flip_horizontal = true,
            .feather = 0.25,
            .strength = 0.37,
        }},
    };
    const auto prepared = image::detail::prepare_warm_retouch_stage(
        adjustment,
        {320U, 180U},
        1.0,
        1.0,
        {.full_dimensions = {320U, 180U}}
    );
    expect(
        prepared.has_value() && prepared->valid() && prepared->regions.size() == 1U,
        "one authored clone stroke lowers into one valid ordered GPU region"
    );
    if (!prepared.has_value() || prepared->regions.empty()) {
        return;
    }
    const auto& region = prepared->regions.front();
    const auto& parameters = region.parameters;
    expect(
        parameters.capsule_count == 2U && std::abs(parameters.donor_offset_x - 12.0F) < 1.0e-5F
            && parameters.donor_offset_y <= 0.0F && parameters.donor_offset_y >= -4.0F
            && std::abs(parameters.source_matrix_xx) < 1.0e-5F
            && std::abs(parameters.source_matrix_xy + 1.5F) < 1.0e-5F
            && std::abs(parameters.source_matrix_yx + 1.5F) < 1.0e-5F
            && std::abs(parameters.source_matrix_yy) < 1.0e-5F && parameters.strength == 0.37F,
        "continuous clone geometry retains capsules, affine donor mapping, and authored strength"
    );
    const auto capsules = records<image::detail::WarmRetouchCapsule>(
        *prepared,
        region.capsule_offset_bytes,
        parameters.capsule_count
    );
    const auto cells = records<image::detail::WarmRetouchCellRange>(
        *prepared,
        region.cell_offset_bytes,
        static_cast<std::size_t>(parameters.grid_columns) * parameters.grid_rows
    );
    const auto references = records<std::uint32_t>(
        *prepared,
        region.reference_offset_bytes,
        parameters.reference_count
    );
    bool complete = true;
    for (std::uint32_t y = parameters.bounds_origin_y;
         y < parameters.bounds_origin_y + parameters.bounds_height;
         ++y) {
        for (std::uint32_t x = parameters.bounds_origin_x;
             x < parameters.bounds_origin_x + parameters.bounds_width;
             ++x) {
            const std::uint32_t column = std::min(
                (x - parameters.bounds_origin_x) * parameters.grid_columns
                    / parameters.bounds_width,
                parameters.grid_columns - 1U
            );
            const std::uint32_t row = std::min(
                (y - parameters.bounds_origin_y) * parameters.grid_rows / parameters.bounds_height,
                parameters.grid_rows - 1U
            );
            const auto range =
                cells[static_cast<std::size_t>(row) * parameters.grid_columns + column];
            for (std::size_t capsule = 0U; capsule < capsules.size(); ++capsule) {
                if (capsule_distance(
                        x,
                        y,
                        capsules[capsule],
                        parameters.radius_x,
                        parameters.radius_y
                    )
                    > 1.0) {
                    continue;
                }
                const auto first = references.begin() + static_cast<std::ptrdiff_t>(range.offset);
                const auto last = first + static_cast<std::ptrdiff_t>(range.count);
                complete =
                    complete && std::find(first, last, static_cast<std::uint32_t>(capsule)) != last;
            }
        }
    }
    expect(
        complete,
        "every covered pixel can reach every contributing capsule through its bounded grid cell"
    );
    expect(
        parameters.reference_count
            < parameters.grid_columns * parameters.grid_rows * parameters.capsule_count,
        "the spatial index omits capsules from cells they cannot cover"
    );
}

void one_point_click_stroke_lowers_to_one_bounded_gpu_capsule() {
    const image::SpotHealAdjustment adjustment{
        .strokes = {{
            .points = {{.x = 0.4, .y = 0.6}},
            .radius_level_zero_pixels = 6U,
            .mode = image::SpotRepairMode::clone,
            .source_offset_x_radii = 2.0,
            .feather = 0.2,
        }},
    };
    const auto prepared = image::detail::prepare_warm_retouch_stage(
        adjustment,
        {200U, 120U},
        1.0,
        1.0,
        {.full_dimensions = {200U, 120U}}
    );
    expect(
        prepared.has_value() && prepared->valid() && prepared->regions.size() == 1U
            && prepared->regions.front().parameters.capsule_count == 1U,
        "a click-authored one-point stroke lowers to one valid GPU region and capsule"
    );
    if (!prepared.has_value() || prepared->regions.empty()) {
        return;
    }
    const auto capsules = records<image::detail::WarmRetouchCapsule>(
        *prepared,
        prepared->regions.front().capsule_offset_bytes,
        1U
    );
    expect(
        capsules.size() == 1U && capsules[0].x0 == capsules[0].x1
            && capsules[0].y0 == capsules[0].y1,
        "a click-authored GPU capsule keeps a circular zero-length centerline"
    );
}

void tile_context_and_heal_mode_are_preserved() {
    const image::SpotHealAdjustment clone{
        .spots = {{
            .center_x = 0.50,
            .center_y = 0.50,
            .radius_level_zero_pixels = 4U,
            .mode = image::SpotRepairMode::clone,
            .source_offset_x_radii = 2.0,
        }},
    };
    const auto tile = image::detail::prepare_warm_retouch_stage(
        clone,
        {40U, 30U},
        1.0,
        1.0,
        {
            .origin_x = 80U,
            .origin_y = 45U,
            .full_dimensions = {200U, 120U},
        }
    );
    expect(
        tile.has_value() && tile->valid() && tile->regions.front().parameters.bounds_origin_x == 14U
            && tile->regions.front().parameters.bounds_origin_y == 9U,
        "detail-tile clone geometry maps normalized full-image coordinates into tile space"
    );

    const image::SpotHealAdjustment heal{
        .spots = {{
            .center_x = 0.5,
            .center_y = 0.5,
            .radius_level_zero_pixels = 4U,
            .mode = image::SpotRepairMode::heal,
        }},
    };
    const auto prepared_heal =
        image::detail::prepare_warm_retouch_stage(heal, {80U, 60U}, 1.0, 1.0, {});
    expect(
        prepared_heal.has_value()
            && prepared_heal->regions.front().parameters.mode
                   == image::detail::WarmRetouchMode::heal
            && prepared_heal->regions.front().parameters.screening_weight == 2.0F
            && prepared_heal->regions.front().parameters.poisson_iterations >= 48U
            && prepared_heal->regions.front().parameters.poisson_iterations <= 192U,
        "Heal lowers with scale-aware bounded reconstruction and the softer seam constraint"
    );

    const image::SpotHealAdjustment edge_clone{
        .spots = {{
            .center_x = 0.95,
            .center_y = 0.5,
            .radius_level_zero_pixels = 4U,
            .mode = image::SpotRepairMode::clone,
            .source_offset_x_radii = 24.0,
        }},
    };
    const auto prepared_edge =
        image::detail::prepare_warm_retouch_stage(edge_clone, {80U, 60U}, 1.0, 1.0, {});
    expect(
        prepared_edge.has_value()
            && prepared_edge->regions.front().parameters.donor_offset_x < 1.0F,
        "an edge donor is translated as one patch instead of repeating clamped edge pixels"
    );
}

} // namespace

int main() {
    continuous_clone_geometry_has_complete_bounded_candidates();
    one_point_click_stroke_lowers_to_one_bounded_gpu_capsule();
    tile_context_and_heal_mode_are_preserved();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
