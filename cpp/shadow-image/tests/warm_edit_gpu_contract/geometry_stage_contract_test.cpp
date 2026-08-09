#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/photo_structural_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"
#include "../../src/proxy/warm_edit_gpu_presentation_surface.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::warm_edit_gpu_contract {

namespace {

using parity_fixture::linear_close;
using parity_fixture::make_random_image;
using parity_fixture::rgb8_difference;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::PhotoGeometry test_geometry() {
    return image::PhotoGeometry{
        .crop_left = 0.08,
        .crop_top = 0.11,
        .crop_right = 0.93,
        .crop_bottom = 0.89,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 6.5,
        .perspective_vertical = 0.3,
        .perspective_horizontal = -0.25,
        .flip_horizontal = true,
    };
}

[[nodiscard]] image::detail::WarmEditGpuRenderContext
geometry_context(
    const image::FloatRgbImage& source,
    const image::PhotoGeometry& geometry,
    const image::PreparedPhotoLiquify* const liquify = nullptr
) {
    const auto layout = image::photo_geometry_layout(source.dimensions, geometry);
    return image::detail::WarmEditGpuRenderContext{
        .geometry = image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect =
                image::GeometryPixelRect{
                    .width = source.dimensions.width,
                    .height = source.dimensions.height,
                },
            .output_rect = image::GeometryPixelRect{
                .width = layout.output_dimensions.width,
                .height = layout.output_dimensions.height,
            },
            .liquify = liquify,
        },
    };
}

[[nodiscard]] std::array<image::AdjustmentNode, 2U> photographic_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "geometry-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.24},
        },
        image::AdjustmentNode{
            .node_id = "geometry-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.87},
        },
    };
}

[[nodiscard]] image::PhotoLiquify test_liquify() {
    return image::PhotoLiquify{
        .strokes = {
            image::PhotoLiquifyPushStroke{
                .points = {
                    {.x = 0.16, .y = 0.31, .pressure = 0.42},
                    {.x = 0.37, .y = 0.38, .pressure = 0.78},
                    {.x = 0.61, .y = 0.35, .pressure = 1.0},
                },
                .radius = 0.14,
                .strength = 0.66,
                .hardness = 0.27,
            },
            image::PhotoLiquifyReconstructStroke{
                .points = {
                    {.x = 0.48, .y = 0.37, .pressure = 0.62},
                    {.x = 0.55, .y = 0.4, .pressure = 0.88},
                },
                .radius = 0.08,
                .strength = 0.43,
                .hardness = 0.39,
            },
            image::PhotoLiquifyPushStroke{
                .points = {
                    {.x = 0.72, .y = 0.73, .pressure = 0.83},
                    {.x = 0.53, .y = 0.59, .pressure = 0.56},
                    {.x = 0.43, .y = 0.48, .pressure = 0.91},
                },
                .radius = 0.1,
                .strength = 0.48,
                .hardness = 0.71,
            },
        },
    };
}

void resident_geometry_matches_the_cpu_oracle() {
    auto source = make_random_image(193U, 127U, false);
    source.level_zero_to_raster_scale_x = 0.5;
    source.level_zero_to_raster_scale_y = 0.25;
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU photo geometry was required but no resident Metal session could be prepared"
        );
        return;
    }
    const auto geometry = test_geometry();
    const auto nodes = photographic_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu =
        preparation.session->render(nodes, plan, true, geometry_context(source, geometry));
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value(),
        "crop, quarter-turn, mirror, straighten and perspective stay in the resident Metal transaction"
    );
    if (!gpu.output || !gpu.output->analyzed_linear) {
        return;
    }

    const auto adjusted = image::execute_adjustment_nodes_with_backend(
        source,
        nodes,
        {.full_dimensions = source.dimensions},
        image::AdjustmentBackendMode::cpu
    );
    const auto cpu = image::apply_photo_geometry(adjusted.pixels, geometry);
    double maximum_error = 0.0;
    const bool parity = linear_close(*gpu.output->analyzed_linear, cpu, maximum_error, 4.0e-4);
    if (!parity) {
        std::cerr << "Geometry warm linear parity max=" << maximum_error << '\n';
    }
    expect(parity, "resident Metal geometry tracks the CPU inverse-map and bilinear oracle");
    const auto cpu_display = image::render_linear_srgb_to_display_srgb8_with_backend(
        cpu,
        {.target_dimensions = cpu.dimensions},
        image::DisplayOutputBackendMode::cpu
    );
    const auto display_difference = rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
    expect(
        display_difference.maximum <= 1U,
        "geometry display output remains within one encoded level of the CPU oracle"
    );
    expect(
        gpu.output->dimensions == cpu.dimensions
            && gpu.output->analyzed_linear->level_zero_to_raster_scale_x == 0.25
            && gpu.output->analyzed_linear->level_zero_to_raster_scale_y == 0.5,
        "transposed geometry returns the authoritative output shape and swapped native scales"
    );

    const auto host_context = geometry_context(source, geometry);
    auto surface_context = host_context;
    surface_context.output_intent =
        image::detail::WarmEditGpuOutputIntent::metal_presentation_surface;
    const auto host = preparation.session->render(
        nodes,
        plan,
        false,
        host_context
    );
    const auto surface = preparation.session->render(
        nodes,
        plan,
        false,
        surface_context
    );
    expect(
        host.output.has_value() && surface.output.has_value()
            && surface.output->rgb8.empty()
            && surface.output->presentation_surface != nullptr
            && surface.output->presentation_surface->materialize_packed_rgb8()
                == host.output->rgb8,
        "geometrized presentation surface is byte-exact with host RGB8"
    );
}

void resident_liquify_and_canvas_match_the_cpu_oracle() {
    const auto source = make_random_image(193U, 127U, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU Liquify was required but no resident Metal session could be prepared"
        );
        return;
    }

    const auto geometry = test_geometry();
    const auto liquify = test_liquify();
    const auto prepared = image::prepare_photo_liquify(source.dimensions, liquify);
    const auto nodes = photographic_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu = preparation.session->render(
        nodes,
        plan,
        true,
        geometry_context(source, geometry, &prepared)
    );
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value(),
        "Push, Reconstruct and Canvas stay in one resident Metal geometry dispatch"
    );
    if (!gpu.output || !gpu.output->analyzed_linear) {
        return;
    }

    const auto adjusted = image::execute_adjustment_nodes_with_backend(
        source,
        nodes,
        {.full_dimensions = source.dimensions},
        image::AdjustmentBackendMode::cpu
    );
    const auto structural =
        image::prepare_photo_structural_rendering(source.dimensions, geometry, &liquify);
    const auto cpu = image::apply_photo_structural_rendering(adjusted.pixels, structural);
    double maximum_error = 0.0;
    const bool parity =
        linear_close(*gpu.output->analyzed_linear, cpu, maximum_error, 8.0e-4);
    if (!parity) {
        std::cerr << "Liquify warm linear parity max=" << maximum_error << '\n';
    }
    expect(
        parity,
        "resident Metal Push/Reconstruct tracks the CPU reverse-stamp and fused Canvas oracle"
    );
    const auto cpu_display = image::render_linear_srgb_to_display_srgb8_with_backend(
        cpu,
        {.target_dimensions = cpu.dimensions},
        image::DisplayOutputBackendMode::cpu
    );
    const auto display_difference = rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
    expect(
        display_difference.maximum <= 1U,
        "Liquify display output remains within one encoded level of the CPU oracle"
    );

    const auto first_stats = preparation.session->stats();
    const auto repeated = preparation.session->render(
        nodes,
        plan,
        false,
        geometry_context(source, geometry, &prepared)
    );
    const auto repeated_stats = preparation.session->stats();
    // A repeated render can rotate to the second resident slot and lazily allocate that slot's
    // geometry intermediates. The cache-hit counter, rather than the aggregate allocation count,
    // is the direct contract for reusing the immutable Liquify candidate table.
    expect(
        repeated.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && repeated.output.has_value()
            && repeated_stats.resource_cache_hit_count
                > first_stats.resource_cache_hit_count,
        "repeated Liquify renders reuse the immutable resident candidate table"
    );
}

void resident_layer_geometry_matches_the_cpu_oracle() {
    const auto source = make_random_image(157U, 101U, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        return;
    }
    const auto geometry = test_geometry();
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "geometry-layer",
            .opacity = 0.72,
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "layer-exposure",
                    .parameters = image::ExposureAdjustment{.stops = -0.31},
                },
                image::AdjustmentNode{
                    .node_id = "layer-contrast",
                    .parameters = image::ContrastAdjustment{
                        .factor = 1.14,
                        .pivot = 0.18,
                    },
                },
            },
        },
    };
    const auto gpu =
        preparation.session->render_layers(layers, true, geometry_context(source, geometry));
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value(),
        "layer composition and photo geometry share one resident Metal command"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu =
            image::apply_photo_geometry(image::execute_adjustment_layers(source, layers), geometry);
        double maximum_error = 0.0;
        const bool parity = linear_close(*gpu.output->analyzed_linear, cpu, maximum_error, 4.0e-4);
        if (!parity) {
            std::cerr << "Layer geometry warm linear parity max=" << maximum_error << '\n';
        }
        expect(parity, "resident Metal layer geometry tracks the complete CPU oracle");
    }

    const auto host_context = geometry_context(source, geometry);
    auto surface_context = host_context;
    surface_context.output_intent =
        image::detail::WarmEditGpuOutputIntent::metal_presentation_surface;
    const auto host =
        preparation.session->render_layers(layers, false, host_context);
    const auto surface =
        preparation.session->render_layers(layers, false, surface_context);
    expect(
        host.output.has_value() && surface.output.has_value()
            && surface.output->rgb8.empty()
            && surface.output->presentation_surface != nullptr
            && surface.output->presentation_surface->materialize_packed_rgb8()
                == host.output->rgb8,
        "layered geometrized presentation surface is byte-exact with host RGB8"
    );
}

template <typename Callable>
[[nodiscard]] double median_milliseconds(const std::size_t iterations, Callable&& callable) {
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        callable();
        const auto finished = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(finished - started).count());
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2U];
}

void benchmark_geometry_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_GEOMETRY_BENCHMARK") == nullptr) {
        return;
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto source = make_random_image(dimensions.width, dimensions.height, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        std::cerr << "BENCH geometry unavailable: " << preparation.diagnostic << '\n';
        ++failures;
        return;
    }
    const auto geometry = test_geometry();
    const auto nodes = photographic_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto context = geometry_context(source, geometry);
    std::uint64_t checksum = 0U;
    const double first_resident = median_milliseconds(1U, [&]() {
        auto rendered = preparation.session->render(nodes, plan, false, context);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    constexpr std::size_t iterations = 3U;
    const double cpu = median_milliseconds(iterations, [&]() {
        auto adjusted = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = dimensions},
            image::AdjustmentBackendMode::cpu
        );
        auto geometrized = image::apply_photo_geometry(adjusted.pixels, geometry);
        auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
            geometrized,
            {.target_dimensions = geometrized.dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        checksum += displayed.bytes[displayed.bytes.size() / 2U];
    });
    const double resident = median_milliseconds(iterations, [&]() {
        auto rendered = preparation.session->render(nodes, plan, false, context);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH photo geometry " << dimensions.width
              << 'x' << dimensions.height << " CPU-adjust+geometry+display=" << cpu << "ms"
              << " first-resident-Metal=" << first_resident << "ms"
              << " resident-Metal=" << resident << "ms"
              << " speedup=" << cpu / resident << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int run_resident_gpu_geometry_contract() {
    failures = 0;
    resident_geometry_matches_the_cpu_oracle();
    resident_liquify_and_canvas_match_the_cpu_oracle();
    resident_layer_geometry_matches_the_cpu_oracle();
    benchmark_geometry_when_requested();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract
