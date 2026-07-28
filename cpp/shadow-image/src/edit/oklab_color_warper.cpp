#include "oklab_color_warper.hpp"

#include "adjustment_node_diagnostics.hpp"
#include "rgb_pixel_traversal.hpp"
#include "working_color_math.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>

namespace shadow::image::detail {

namespace {

[[nodiscard]] double smooth_transition(const double lower, const double upper,
                                       const double value) noexcept {
    if (value <= lower) {
        return 0.0;
    }
    if (value >= upper) {
        return 1.0;
    }
    const double normalized = (value - lower) / (upper - lower);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

[[nodiscard]] double color_warper_displacement(const OklabColorWarperAdjustment& parameters,
                                               const std::size_t row, const std::size_t column,
                                               const bool a_axis) noexcept {
    const OklabColorWarperControlPoint& point =
        parameters.control_points.at(row * oklab_color_warper_grid_side + column);
    return a_axis ? point.a_offset : point.b_offset;
}

Vector3 apply_oklab_color_warper_pixel(const Vector3& input,
                                       const OklabColorWarperAdjustment& parameters,
                                       const WorkingSpaceTransform& color_transform) noexcept {
    Vector3 lab = working_rgb_to_oklab(color_transform, input);
    const double half_extent = oklab_color_warper_half_extent;
    const double a_distance = half_extent - std::abs(lab[1]);
    const double b_distance = half_extent - std::abs(lab[2]);
    const double coverage = smooth_transition(0.0, oklab_color_warper_edge_feather, a_distance) *
                            smooth_transition(0.0, oklab_color_warper_edge_feather, b_distance);
    if (coverage == 0.0 || parameters.strength == 0.0) {
        return input;
    }

    const double coordinate_scale = static_cast<double>(oklab_color_warper_grid_side - 1U);
    const double grid_a = std::clamp(
        (lab[1] + half_extent) / (2.0 * half_extent) * coordinate_scale, 0.0, coordinate_scale);
    const double grid_b = std::clamp(
        (lab[2] + half_extent) / (2.0 * half_extent) * coordinate_scale, 0.0, coordinate_scale);
    const std::size_t left = static_cast<std::size_t>(std::floor(grid_a));
    const std::size_t top = static_cast<std::size_t>(std::floor(grid_b));
    const std::size_t right = std::min(left + 1U, oklab_color_warper_grid_side - 1U);
    const std::size_t bottom = std::min(top + 1U, oklab_color_warper_grid_side - 1U);
    const double horizontal = grid_a - static_cast<double>(left);
    const double vertical = grid_b - static_cast<double>(top);
    const auto bilinear = [=, &parameters](const bool a_axis) {
        const double top_value =
            std::lerp(color_warper_displacement(parameters, top, left, a_axis),
                      color_warper_displacement(parameters, top, right, a_axis), horizontal);
        const double bottom_value =
            std::lerp(color_warper_displacement(parameters, bottom, left, a_axis),
                      color_warper_displacement(parameters, bottom, right, a_axis), horizontal);
        return std::lerp(top_value, bottom_value, vertical);
    };
    const double amount = coverage * parameters.strength;
    lab[1] += amount * bilinear(true);
    lab[2] += amount * bilinear(false);
    return oklab_to_working_rgb(color_transform, lab);
}

} // namespace

void validate_oklab_color_warper(const OklabColorWarperAdjustment& parameters,
                                 const AdjustmentNode& node, const std::size_t node_index) {
    const bool valid_control_points = std::ranges::all_of(
        parameters.control_points, [](const OklabColorWarperControlPoint& point) {
            return std::isfinite(point.a_offset) &&
                   std::abs(point.a_offset) <= oklab_color_warper_maximum_offset &&
                   std::isfinite(point.b_offset) &&
                   std::abs(point.b_offset) <= oklab_color_warper_maximum_offset;
        });
    if (!std::isfinite(parameters.strength) || parameters.strength < 0.0 ||
        parameters.strength > 1.0 || !valid_control_points) {
        throw_node_error(
            EditErrorCode::invalid_parameter, node_index, node,
            "Oklab Color Warper controls must be finite and within their declared bounds");
    }
}

bool oklab_color_warper_is_neutral(const OklabColorWarperAdjustment& parameters) noexcept {
    return parameters.strength == 0.0 ||
           std::ranges::all_of(parameters.control_points,
                               [](const OklabColorWarperControlPoint& point) {
                                   return point.a_offset == 0.0 && point.b_offset == 0.0;
                               });
}

void apply_oklab_color_warper_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                  const std::size_t node_index,
                                  const OklabColorWarperAdjustment& parameters) {
    if (oklab_color_warper_is_neutral(parameters)) {
        return;
    }
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    transform_rgb_pixels(
        image, node_index, node, [&parameters, &color_transform](const Vector3& input) {
            return apply_oklab_color_warper_pixel(input, parameters, color_transform);
        });
}

} // namespace shadow::image::detail
