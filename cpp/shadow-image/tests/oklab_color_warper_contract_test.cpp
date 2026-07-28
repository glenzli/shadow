#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"
#include "metal_adjustment_program.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

[[nodiscard]] image::AdjustmentNode
color_warper_node(std::string id, image::OklabColorWarperAdjustment parameters) {
    return image::AdjustmentNode{
        .node_id = std::move(id),
        .parameter_schema_version = image::oklab_color_warper_parameter_schema_version,
        .implementation_version = image::oklab_color_warper_implementation_version,
        .parameters = std::move(parameters),
    };
}

[[nodiscard]] image::OklabColorWarperAdjustment uniform_warper() {
    image::OklabColorWarperAdjustment parameters;
    for (auto& point : parameters.control_points) {
        point.a_offset = 0.042;
        point.b_offset = -0.028;
    }
    parameters.strength = 0.72;
    return parameters;
}

[[nodiscard]] image::FloatRgbImage lattice_input() {
    constexpr std::uint32_t width = 13U;
    constexpr std::uint32_t height = 7U;
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const double hue = 360.0 * static_cast<double>(x) / width;
            const double chroma = 0.04 + 0.018 * static_cast<double>(y);
            const auto rgb = linear_srgb_from_oklch(0.42 + 0.04 * y, chroma, hue);
            samples.insert(samples.end(), rgb.begin(), rgb.end());
        }
    }
    auto input = rgb_raster(width, height, std::move(samples));
    input.working_space = linear_srgb();
    return input;
}

[[nodiscard]] bool close_to_cpu(const image::FloatRgbImage& actual,
                                const image::FloatRgbImage& expected,
                                const double relative_tolerance) {
    if (actual.dimensions != expected.dimensions ||
        actual.row_stride_bytes != expected.row_stride_bytes ||
        actual.samples.size() != expected.samples.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.samples.size(); ++index) {
        const double reference = expected.samples[index];
        const double difference = std::abs(static_cast<double>(actual.samples[index]) - reference);
        const double tolerance = relative_tolerance * std::max(1.0, std::abs(reference));
        if (difference > tolerance) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::array<float, 3U> rgb_from_oklab_axes(const double lightness, const double a,
                                                        const double b) {
    constexpr double pi = 3.141592653589793238462643383279502884;
    double hue = std::atan2(b, a) * 180.0 / pi;
    if (hue < 0.0) {
        hue += 360.0;
    }
    return linear_srgb_from_oklch(lightness, std::hypot(a, b), hue);
}

[[nodiscard]] std::array<double, 3U>
render_one_lab(const std::array<float, 3U>& source,
               const image::OklabColorWarperAdjustment& parameters) {
    auto input = rgb_image(1U, {source[0], source[1], source[2]});
    input.working_space = linear_srgb();
    const std::array nodes{color_warper_node("numeric-color-warper", parameters)};
    const auto output = image::execute_adjustment_nodes(input, nodes);
    return oklab_from_linear_srgb({output.samples[0], output.samples[1], output.samples[2]});
}

void color_warper_has_a_pixel_local_backend_contract() {
    const auto input = lattice_input();
    const std::array nodes{
        color_warper_node("connected-oklab-color-warp", uniform_warper()),
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto cpu = image::execute_adjustment_nodes(input, nodes);
    expect(plan.segments.size() == 1U &&
               plan.segments.front().locality == image::AdjustmentLocality::pixel_local &&
               plan.segments.front().steps ==
                   std::vector{image::EditExecutionStep{
                       .node_index = 0U,
                       .operation = image::AdjustmentOperation::oklab_color_warper,
                   }} &&
               cpu.samples != input.samples,
           "the Oklab Color Warper is an observable pixel-local lattice operation");
    expect(image::operation_id(image::AdjustmentOperation::oklab_color_warper) ==
               "shadow.oklab_color_warper",
           "the Oklab Color Warper exposes a stable operation identity");

    const auto automatic = image::execute_adjustment_nodes_with_backend(
        input, nodes, {}, image::AdjustmentBackendMode::automatic);
    if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        expect(automatic.backend == image::AdjustmentBackend::metal && !automatic.fell_back &&
                   close_to_cpu(automatic.pixels, cpu, 2.0e-4),
               "automatic Metal Color Warper matches the CPU lattice oracle");
        const auto forced = image::execute_adjustment_nodes_with_backend(
            input, nodes, {}, image::AdjustmentBackendMode::metal);
        expect(forced.backend == image::AdjustmentBackend::metal && !forced.fell_back &&
                   close_to_cpu(forced.pixels, cpu, 2.0e-4),
               "forced Metal Color Warper remains within CPU parity tolerance");
    } else {
        expect(automatic.backend == image::AdjustmentBackend::cpu && automatic.fell_back &&
                   automatic.pixels.samples == cpu.samples,
               "Color Warper performs a complete CPU replay when Metal is unavailable");
    }

    const std::array neutral_nodes{
        color_warper_node("neutral-oklab-color-warp", image::OklabColorWarperAdjustment{})};
    expect(image::compile_edit_execution_plan(neutral_nodes).segments.empty(),
           "the default Color Warper lattice is exactly neutral");
    auto zero_strength = uniform_warper();
    zero_strength.strength = 0.0;
    const std::array zero_strength_nodes{
        color_warper_node("zero-strength-oklab-color-warp", std::move(zero_strength))};
    expect(image::compile_edit_execution_plan(zero_strength_nodes).segments.empty(),
           "zero strength elides an authored lattice without altering its geometry");
}

void color_warper_interpolates_row_major_axes_and_strength() {
    constexpr std::size_t row = 1U;
    constexpr std::size_t column = 3U;
    image::OklabColorWarperAdjustment parameters;
    parameters.strength = 0.60;
    parameters.control_points[row * image::oklab_color_warper_grid_side + column] = {
        .a_offset = 0.052,
        .b_offset = -0.031,
    };
    parameters.control_points[column * image::oklab_color_warper_grid_side + row] = {
        .a_offset = -0.047,
        .b_offset = 0.024,
    };
    const double grid_step = 2.0 * image::oklab_color_warper_half_extent /
                             static_cast<double>(image::oklab_color_warper_grid_side - 1U);
    const double source_a = -image::oklab_color_warper_half_extent + grid_step * column;
    const double source_b = -image::oklab_color_warper_half_extent + grid_step * row;
    const auto source = rgb_from_oklab_axes(0.64, source_a, source_b);
    const auto actual = render_one_lab(source, parameters);
    expect_close_double(actual[0], 0.64, 2.0e-5, "Color Warper preserves Oklab lightness");
    expect_close_double(actual[1], source_a + 0.60 * 0.052, 2.0e-5,
                        "Color Warper columns address the Oklab a axis");
    expect_close_double(actual[2], source_b + 0.60 * -0.031, 2.0e-5,
                        "Color Warper rows address the Oklab b axis");
}

void color_warper_has_an_explicit_boundary_feather() {
    image::OklabColorWarperAdjustment parameters;
    parameters.strength = 1.0;
    for (auto& point : parameters.control_points) {
        point.a_offset = 0.04;
    }

    const auto interior = render_one_lab(rgb_from_oklab_axes(0.62, 0.26, 0.0), parameters);
    expect_close_double(interior[1], 0.30, 2.0e-5,
                        "Color Warper applies the full displacement inside its mesh");

    const auto feather = render_one_lab(rgb_from_oklab_axes(0.62, 0.30, 0.0), parameters);
    expect_close_double(feather[1], 0.32, 2.0e-5,
                        "Color Warper reaches half displacement at the feather midpoint");

    const auto outside_source = rgb_from_oklab_axes(0.62, 0.34, 0.0);
    auto outside_input = rgb_image(1U, {outside_source[0], outside_source[1], outside_source[2]});
    outside_input.working_space = linear_srgb();
    const std::array outside_nodes{
        color_warper_node("outside-color-warper", parameters),
    };
    const auto outside = image::execute_adjustment_nodes(outside_input, outside_nodes);
    expect(outside.samples == outside_input.samples,
           "Color Warper exactly bypasses colors outside its declared mesh");
}

void color_warper_validation_precedes_elision() {
    auto malformed = uniform_warper();
    malformed.control_points.front().a_offset = image::oklab_color_warper_maximum_offset + 0.001;

    auto wrong_version = color_warper_node("wrong-version-color-warper", malformed);
    wrong_version.parameter_schema_version = 0U;
    wrong_version.implementation_version = 0U;
    expect_edit_error([&] { image::validate_adjustment_nodes(std::array{wrong_version}); },
                      image::EditErrorCode::unsupported_version, 0U,
                      "Color Warper rejects its version before malformed parameters");

    auto disabled = color_warper_node("disabled-malformed-color-warper", malformed);
    disabled.enabled = false;
    expect_edit_error([&] { image::validate_adjustment_nodes(std::array{disabled}); },
                      image::EditErrorCode::invalid_parameter, 0U,
                      "disabled Color Warper nodes still validate every control point");

    malformed.strength = 0.0;
    expect_edit_error(
        [&] {
            static_cast<void>(image::compile_edit_execution_plan(std::array{
                color_warper_node("zero-malformed-color-warper", malformed),
            }));
        },
        image::EditErrorCode::invalid_parameter, 0U,
        "zero-strength Color Warper nodes validate before neutral elision");

    auto non_finite = uniform_warper();
    non_finite.strength = std::numeric_limits<double>::quiet_NaN();
    expect_edit_error(
        [&] {
            image::validate_adjustment_nodes(std::array{
                color_warper_node("non-finite-color-warper", non_finite),
            });
        },
        image::EditErrorCode::invalid_parameter, 0U, "Color Warper rejects non-finite strength");
}

void color_warper_host_lowering_preserves_the_lattice_contract() {
    image::OklabColorWarperAdjustment parameters;
    parameters.strength = 0.72;
    for (std::size_t index = 0U; index < parameters.control_points.size(); ++index) {
        parameters.control_points[index].a_offset = 0.001 * static_cast<double>(index + 1U);
        parameters.control_points[index].b_offset = -0.0005 * static_cast<double>(index + 1U);
    }
    const std::array nodes{
        color_warper_node("lowered-color-warper", parameters),
    };
    const auto input = lattice_input();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto preparation = image::detail::prepare_metal_adjustment(input, nodes, plan, {});
    expect(preparation.program.has_value(),
           "valid Color Warper prepares a device-independent Metal program");
    if (!preparation.program.has_value()) {
        return;
    }
    const auto& program = *preparation.program;
    expect(program.operations.size() == 1U && program.invocation.step_count == 1U &&
               program.invocation.perceptual_mixer_entry_count ==
                   image::oklab_color_warper_control_point_count &&
               program.perceptual_mixer_entries.size() ==
                   image::oklab_color_warper_control_point_count,
           "Color Warper lowering reserves exactly one row-major lattice");
    const auto& operation = program.operations.front();
    expect(operation.opcode == static_cast<std::uint32_t>(
                                   image::detail::MetalAdjustmentOpcode::oklab_color_warper) &&
               operation.resource_offset == 0U &&
               operation.resource_count == image::oklab_color_warper_control_point_count &&
               operation.parameter_0 == std::array{0.72F, 0.32F, 0.04F, 0.0F},
           "Color Warper lowering seals strength, extent, and feather semantics");
    for (const std::size_t index : std::array<std::size_t, 3U>{0U, 12U, 24U}) {
        expect(program.perceptual_mixer_entries[index].value ==
                   std::array{
                       static_cast<float>(parameters.control_points[index].a_offset),
                       static_cast<float>(parameters.control_points[index].b_offset),
                       0.0F,
                       0.0F,
                   },
               "Color Warper lowering preserves row-major a/b control bytes");
    }
}

} // namespace

int main() {
    color_warper_has_a_pixel_local_backend_contract();
    color_warper_interpolates_row_major_axes_and_strength();
    color_warper_has_an_explicit_boundary_feather();
    color_warper_validation_precedes_elision();
    color_warper_host_lowering_preserves_the_lattice_contract();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
