#include "contract_test_assertions.hpp"

#include <warm_edit_gpu_brush_index.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

[[nodiscard]] double capsule_distance(
    const image::detail::WarmBrushCapsule capsule,
    const double x,
    const double y,
    const image::Dimensions full
) {
    const double shorter = static_cast<double>(std::min(full.width, full.height));
    const double scale_x = static_cast<double>(full.width) / shorter;
    const double scale_y = static_cast<double>(full.height) / shorter;
    const double point_x = x * scale_x;
    const double point_y = y * scale_y;
    const double start_x = capsule.x0 * scale_x;
    const double start_y = capsule.y0 * scale_y;
    const double dx = (capsule.x1 - capsule.x0) * scale_x;
    const double dy = (capsule.y1 - capsule.y0) * scale_y;
    const double denominator = std::fma(dx, dx, dy * dy);
    if (denominator <= std::numeric_limits<double>::epsilon()) {
        return std::hypot(point_x - start_x, point_y - start_y);
    }
    const double projection =
        std::clamp(((point_x - start_x) * dx + (point_y - start_y) * dy) / denominator, 0.0, 1.0);
    return std::hypot(
        point_x - std::fma(projection, dx, start_x),
        point_y - std::fma(projection, dy, start_y)
    );
}

[[nodiscard]] std::vector<std::uint32_t>
candidates(const image::detail::WarmGpuBrushIndex& index, const double x, const double y) {
    const auto column = std::min(
        static_cast<std::uint32_t>(std::clamp(x, 0.0, 1.0) * index.grid_columns),
        index.grid_columns - 1U
    );
    const auto row = std::min(
        static_cast<std::uint32_t>(std::clamp(y, 0.0, 1.0) * index.grid_rows),
        index.grid_rows - 1U
    );
    const auto range = index.cell(row * index.grid_columns + column);
    std::vector<std::uint32_t> result;
    result.reserve(range.count);
    for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
        result.push_back(index.reference(range.offset + offset));
    }
    return result;
}

void stroke_breaks_create_segments_without_disconnected_dot_or_bridge_approximations() {
    image::LocalMask mask{
        .kind = image::LocalMaskKind::brush,
        .radius_x = 0.04,
        .feather = 0.5,
        .points = {
            {.x = 0.10, .y = 0.20, .begins_stroke = true},
            {.x = 0.30, .y = 0.25},
            {.x = 0.55, .y = 0.40},
            {.x = 0.72, .y = 0.70, .begins_stroke = true},
            {.x = 0.88, .y = 0.82, .begins_stroke = true},
        },
    };
    constexpr image::Dimensions full{1'200U, 800U};
    const auto preparation = image::detail::prepare_warm_gpu_brush_index(mask, full);
    expect(
        preparation.index.has_value() && preparation.index->valid(),
        "a multi-stroke brush produces one valid bounded spatial index"
    );
    if (!preparation.index) {
        return;
    }
    const auto& index = *preparation.index;
    expect(
        index.capsule_count == 4U,
        "three connected points become two continuous capsules and two isolated strokes stay dots"
    );
    const auto first = index.capsule(0U);
    const auto second = index.capsule(1U);
    const auto isolated = index.capsule(2U);
    expect(
        first.x0 == 0.10F && first.x1 == 0.30F && second.x0 == 0.30F && second.x1 == 0.55F,
        "connected samples preserve their authored segment chain"
    );
    expect(
        isolated.x0 == isolated.x1 && isolated.y0 == isolated.y1,
        "a stroke break never invents a bridge from the previous stroke"
    );
}

void every_capsule_that_can_cover_a_pixel_is_present_in_that_pixels_grid_cell() {
    image::LocalMask mask{
        .kind = image::LocalMaskKind::brush,
        .radius_x = 0.065,
        .feather = 0.35,
        .points = {
            {.x = 0.02, .y = 0.12, .begins_stroke = true},
            {.x = 0.25, .y = 0.28},
            {.x = 0.63, .y = 0.46},
            {.x = 0.96, .y = 0.91},
            {.x = 0.82, .y = 0.12, .begins_stroke = true},
        },
    };
    constexpr image::Dimensions full{1'500U, 900U};
    const auto preparation = image::detail::prepare_warm_gpu_brush_index(mask, full);
    expect(preparation.index.has_value(), "candidate-completeness brush index prepares");
    if (!preparation.index) {
        return;
    }
    const auto& index = *preparation.index;
    bool complete = true;
    for (std::uint32_t row = 0U; row < 23U; ++row) {
        const double y = (static_cast<double>(row) + 0.5) / 23.0;
        for (std::uint32_t column = 0U; column < 37U; ++column) {
            const double x = (static_cast<double>(column) + 0.5) / 37.0;
            const auto cell_candidates = candidates(index, x, y);
            for (std::uint32_t capsule = 0U; capsule < index.capsule_count; ++capsule) {
                if (capsule_distance(index.capsule(capsule), x, y, full) <= mask.radius_x
                    && std::ranges::find(cell_candidates, capsule) == cell_candidates.end()) {
                    complete = false;
                }
            }
        }
    }
    expect(
        complete,
        "the CSR grid never culls a continuous capsule that can influence a sampled pixel"
    );
}

void long_diagonal_segments_are_rasterized_into_nearby_cells_not_their_complete_aabb() {
    const image::LocalMask mask{
        .kind = image::LocalMaskKind::brush,
        .radius_x = 0.01,
        .feather = 0.5,
        .points = {
            {.x = 0.0, .y = 0.0, .begins_stroke = true},
            {.x = 1.0, .y = 1.0},
        },
    };
    constexpr image::Dimensions full{1'024U, 1'024U};
    const auto preparation = image::detail::prepare_warm_gpu_brush_index(mask, full);
    expect(
        preparation.index.has_value() && preparation.index->reference_count < 256U,
        "a long diagonal capsule occupies a narrow band of cells instead of its full bounding box"
    );
}

} // namespace

int main() {
    stroke_breaks_create_segments_without_disconnected_dot_or_bridge_approximations();
    every_capsule_that_can_cover_a_pixel_is_present_in_that_pixels_grid_cell();
    long_diagonal_segments_are_rasterized_into_nearby_cells_not_their_complete_aabb();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
