#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include "../scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"
#include "../../src/proxy/warm_edit_gpu_presentation_surface.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::warm_edit_gpu_contract {

namespace {

using parity_fixture::linear_close;
using parity_fixture::linear_srgb;
using parity_fixture::make_random_image;
using parity_fixture::rgb8_difference;
using shadow::image::test_support::ScopedEnvironment;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::array<image::AdjustmentNode, 4U> core_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "white-balance",
            .parameters =
                image::RgbWhiteBalanceAdjustment{
                    .temperature = 0.24,
                    .tint = -0.13,
                },
        },
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.37},
        },
        image::AdjustmentNode{
            .node_id = "contrast",
            .parameters =
                image::ContrastAdjustment{
                    .factor = 1.42,
                    .pivot = 0.18,
                },
        },
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.76},
        },
    };
}

[[nodiscard]] image::FloatRgbImage make_boundary_image() {
    image::FloatRgbImage result{
        .dimensions = {5U, 1U},
        .row_stride_bytes = 5U * 3U * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_srgb(),
        .samples = {
            0.0F,
            0.0F,
            0.0F,
            1.0F,
            1.0F,
            1.0F,
            1.0F,
            0.0F,
            0.0F,
            0.0F,
            1.0F,
            0.0F,
            0.0F,
            0.0F,
            1.0F,
        },
    };
    return result;
}

void host_replay_bytes_are_exact_and_idempotent() {
    const auto source = make_random_image(41U, 29U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        return;
    }
    const auto before = preparation.session->stats();
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "host-replay-neutral",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto ordinary = preparation.session->render(
        neutral_nodes,
        image::compile_edit_execution_plan(neutral_nodes),
        false
    );
    const auto after_ordinary = preparation.session->stats();
    const auto first = preparation.session->host_source_for_cpu_replay();
    const auto after_first = preparation.session->stats();
    const auto repeated = preparation.session->host_source_for_cpu_replay();
    const auto after_repeated = preparation.session->stats();
    const std::uint64_t expected_host_bytes =
        static_cast<std::uint64_t>(source.row_stride_bytes) * source.dimensions.height;
    expect(
        ordinary.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && ordinary.output.has_value()
            && after_ordinary.resident_bytes == before.resident_bytes,
        "ordinary resident Metal rendering does not precharge a host replay"
    );
    expect(
        first.source != nullptr && !first.cancelled && first.diagnostic.empty()
            && after_first.resident_bytes == after_ordinary.resident_bytes + expected_host_bytes,
        "the first CPU replay materialization adds its exact padded fp32 source bytes"
    );
    expect(
        repeated.source == first.source && !repeated.cancelled && repeated.diagnostic.empty()
            && after_repeated.resident_bytes == after_first.resident_bytes,
        "repeated CPU replay reuses one host source without charging it twice"
    );
}

void resident_backend_matches_cpu_oracle() {
    const auto source = make_random_image(257U, 129U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        if (std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") != nullptr) {
            std::cerr << "Warm Metal preparation: " << preparation.diagnostic << '\n';
        }
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "session-resident warm Metal was required but could not be prepared"
        );
        return;
    }
    const auto initial_stats = preparation.session->stats();
    expect(
        initial_stats.resident && initial_stats.source_upload_count == 1U
            && initial_stats.gpu_buffer_allocation_count == 10U && initial_stats.render_count == 0U,
        "resident backend starts with one source, one dummy side table, and two four-buffer slots"
    );

    auto original_nodes = core_nodes();
    std::array<std::size_t, 4U> order{0U, 1U, 2U, 3U};
    std::size_t permutation_count = 0U;
    do {
        std::array<image::AdjustmentNode, 4U> nodes{
            original_nodes[order[0]],
            original_nodes[order[1]],
            original_nodes[order[2]],
            original_nodes[order[3]],
        };
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu_adjusted = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto cpu_display = image::render_linear_srgb_to_display_srgb8_with_backend(
            cpu_adjusted.pixels,
            {.target_dimensions = source.dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        auto gpu = preparation.session->render(nodes, plan, true);
        expect(
            gpu.output.has_value() && gpu.output->analyzed_linear.has_value(),
            "every supported node order completes on the resident backend"
        );
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            continue;
        }
        expect(
            gpu.output->analyzed_linear->row_stride_bytes
                    == static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float)
                && gpu.output->analyzed_linear->samples.size()
                       == static_cast<std::size_t>(source.dimensions.pixel_count()) * 3U,
            "resident analyzed output is packed and contains no unwritten source padding"
        );
        double maximum_linear_error = 0.0;
        const auto rgb8 = rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
        const bool linear_matches =
            linear_close(*gpu.output->analyzed_linear, cpu_adjusted.pixels, maximum_linear_error);
        if (!linear_matches) {
            std::cerr << "Warm linear parity order=" << order[0] << order[1] << order[2] << order[3]
                      << " max=" << maximum_linear_error << '\n';
        }
        expect(
            linear_matches,
            "resident fused linear output stays within the established fp32 tolerance"
        );
        expect(
            rgb8.maximum <= 1U && rgb8.p99 == 0U,
            "resident fused display output differs by at most one code with p99 exact"
        );
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));
    expect(permutation_count == 24U, "all 24 core-operation orders were validated");

    const auto boundary = make_boundary_image();
    auto boundary_preparation = image::detail::prepare_warm_edit_gpu_session(boundary);
    expect(
        boundary_preparation.session != nullptr,
        "boundary source can prepare a resident session"
    );
    if (boundary_preparation.session) {
        const std::array neutral_nodes{
            image::AdjustmentNode{
                .node_id = "neutral",
                .parameters = image::ExposureAdjustment{},
            },
        };
        const auto neutral_plan = image::compile_edit_execution_plan(neutral_nodes);
        const auto cpu_display = image::render_linear_srgb_to_display_srgb8_with_backend(
            boundary,
            {.target_dimensions = boundary.dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        const auto gpu = boundary_preparation.session->render(neutral_nodes, neutral_plan, true);
        expect(
            gpu.output.has_value() && !gpu.output->had_active_adjustments
                && gpu.output->analyzed_linear.has_value()
                && rgb8_difference(gpu.output->rgb8, cpu_display.bytes).maximum <= 1U,
            "neutral boundary display stays within one RGB8 code and records no adjustment"
        );
    }

    const auto allocation_snapshot = preparation.session->stats();
    const auto nodes = core_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    auto first = std::async(std::launch::async, [&]() {
        return preparation.session->render(nodes, plan, false);
    });
    auto second = std::async(std::launch::async, [&]() {
        return preparation.session->render(nodes, plan, false);
    });
    const auto first_result = first.get();
    const auto second_result = second.get();
    const auto after_concurrent = preparation.session->stats();
    expect(
        first_result.output.has_value() && second_result.output.has_value()
            && first_result.output->rgb8 == second_result.output->rgb8
            && !first_result.output->analyzed_linear.has_value()
            && !second_result.output->analyzed_linear.has_value()
            && after_concurrent.source_upload_count == allocation_snapshot.source_upload_count
            && after_concurrent.gpu_buffer_allocation_count
                   == allocation_snapshot.gpu_buffer_allocation_count
            && after_concurrent.render_count == allocation_snapshot.render_count + 2U
            && after_concurrent.completed_render_count
                   == allocation_snapshot.completed_render_count + 2U
            && after_concurrent.peak_concurrent_renders >= 1U
            && after_concurrent.peak_concurrent_renders <= 2U,
        "concurrent no-analysis renders reuse fixed slots and skip linear readback"
    );

    const auto surface_source = make_random_image(17U, 5U, false);
    auto surface_preparation = image::detail::prepare_warm_edit_gpu_session(surface_source);
    expect(
        surface_preparation.session != nullptr,
        "presentation-surface source can prepare a resident session"
    );
    if (surface_preparation.session) {
        const std::array neutral_nodes{
            image::AdjustmentNode{
                .node_id = "neutral",
                .parameters = image::ExposureAdjustment{},
            },
        };
        const auto neutral_plan = image::compile_edit_execution_plan(neutral_nodes);
        const auto cpu_display = image::render_linear_srgb_to_display_srgb8_with_backend(
            surface_source,
            {.target_dimensions = surface_source.dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        const image::detail::WarmEditGpuRenderContext presentation_context{
            .output_intent = image::detail::WarmEditGpuOutputIntent::metal_presentation_surface,
        };
        const auto surface_stats_before = surface_preparation.session->stats();
        const auto host_surface_oracle =
            surface_preparation.session->render(neutral_nodes, neutral_plan, false);
        auto first_surface_attempt =
            surface_preparation.session
                ->render(neutral_nodes, neutral_plan, false, presentation_context);
        expect(
            first_surface_attempt.output.has_value() && first_surface_attempt.output->rgb8.empty()
                && first_surface_attempt.output->presentation_surface != nullptr,
            "interactive Metal intent publishes an owned surface without host RGB8"
        );
        if (first_surface_attempt.output.has_value()
            && first_surface_attempt.output->presentation_surface) {
            const auto first_surface = first_surface_attempt.output->presentation_surface;
            const auto first_rgb8 = first_surface->materialize_packed_rgb8();
            const auto first_rgba = first_surface->rgba8_rows();
            expect(
                first_surface->dimensions() == surface_source.dimensions
                    && first_surface->row_stride_bytes() >= 17U * 4U
                    && first_surface->row_stride_bytes() % 256U == 0U
                    && first_surface->pixel_format()
                           == image::detail::WarmEditGpuPresentationPixelFormat::
                               rgba8_unorm_display_srgb
                    && first_surface->resource_id() != 0U
                    && first_surface->native_texture_handle() != 0U
                    && first_surface->native_device_handle() != 0U,
                "presentation surface has an independent aligned display-sRGB Metal descriptor"
            );
            expect(
                host_surface_oracle.output.has_value()
                    && first_rgb8 == host_surface_oracle.output->rgb8,
                "presentation pack is byte-exact with the same resident host RGB8 render"
            );
            expect(
                rgb8_difference(first_rgb8, cpu_display.bytes).maximum <= 1U,
                "presentation surface materializes the same RGB8 codes as the CPU oracle"
            );
            bool alpha_is_opaque = true;
            for (std::uint32_t row = 0U; row < surface_source.dimensions.height; ++row) {
                for (std::uint32_t column = 0U; column < surface_source.dimensions.width;
                     ++column) {
                    const auto offset =
                        static_cast<std::size_t>(row) * first_surface->row_stride_bytes()
                        + static_cast<std::size_t>(column) * 4U + 3U;
                    alpha_is_opaque = alpha_is_opaque && first_rgba[offset] == 255U;
                }
            }
            expect(alpha_is_opaque, "presentation surface writes an opaque alpha channel");

            std::array<std::uint64_t, 3U> later_ids{};
            for (std::size_t index = 0U; index < later_ids.size(); ++index) {
                auto later = surface_preparation.session
                                 ->render(neutral_nodes, neutral_plan, false, presentation_context);
                expect(
                    later.output.has_value() && later.output->presentation_surface != nullptr,
                    "later presentation render publishes an owned surface"
                );
                if (later.output && later.output->presentation_surface) {
                    later_ids[index] = later.output->presentation_surface->resource_id();
                }
            }
            expect(
                std::ranges::none_of(
                    later_ids,
                    [first_id = first_surface->resource_id()](const auto id) {
                        return id == 0U || id == first_id;
                    }
                ) && first_surface->materialize_packed_rgb8() == first_rgb8,
                "holding more than two presentation frames neither aliases nor pins render slots"
            );
        }

        std::stop_source surface_cancellation;
        static_cast<void>(surface_cancellation.request_stop());
        const auto cancelled_surface = surface_preparation.session->render(
            neutral_nodes,
            neutral_plan,
            false,
            presentation_context,
            surface_cancellation.get_token()
        );
        expect(
            cancelled_surface.status == image::detail::WarmEditGpuSession::RenderStatus::cancelled
                && !cancelled_surface.output.has_value(),
            "cancelled presentation render publishes no native surface"
        );
        const ScopedEnvironment injected_surface_failure(
            "SHADOW_TEST_WARM_METAL_FORCE_FAILURE",
            "1"
        );
        const auto failed_surface =
            surface_preparation.session
                ->render(neutral_nodes, neutral_plan, false, presentation_context);
        expect(
            failed_surface.status
                    == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
                && !failed_surface.output.has_value(),
            "failed presentation render publishes no native surface"
        );
        const auto surface_stats_after = surface_preparation.session->stats();
        expect(
            surface_stats_after.presentation_surface_request_count
                    == surface_stats_before.presentation_surface_request_count + 4U
                && surface_stats_after.presentation_surface_publish_count
                       == surface_stats_before.presentation_surface_publish_count + 4U
                && surface_stats_after.presentation_surface_fallback_count
                       == surface_stats_before.presentation_surface_fallback_count,
            "presentation telemetry distinguishes requests, publishes, and host fallbacks"
        );
    }

    {
        const auto before_failure = preparation.session->stats();
        const ScopedEnvironment injected_failure("SHADOW_TEST_WARM_METAL_FORCE_FAILURE", "1");
        const auto failure = preparation.session->render(nodes, plan, false);
        const auto after_failure = preparation.session->stats();
        expect(
            !failure.output.has_value() && !failure.diagnostic.empty()
                && after_failure.render_count == before_failure.render_count
                && after_failure.gpu_buffer_allocation_count
                       == before_failure.gpu_buffer_allocation_count,
            "injected warm failure declines before a slot and never allocates transient buffers"
        );
    }
}

void accepted_completion_stays_on_the_resident_preview_path() {
    const auto source = make_random_image(257U, 129U, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "AI completion requires a resident Metal session for this contract"
        );
        return;
    }
    image::ImageCompletionPatch patch{
        .raster_width = 16U,
        .raster_height = 16U,
        .coordinate_width = source.dimensions.width,
        .coordinate_height = source.dimensions.height,
        .bounds_left = 0.23,
        .bounds_top = 0.18,
        .bounds_right = 0.76,
        .bounds_bottom = 0.82,
        .strength = 1.0,
        .rgba8 = std::vector<std::uint8_t>(16U * 16U * 4U, 175U),
    };
    for (std::size_t pixel = 0U; pixel < 256U; ++pixel) {
        patch.rgba8[pixel * 4U + 3U] = pixel % 5U == 0U ? 96U : 255U;
    }
    std::array nodes{
        image::AdjustmentNode{
            .node_id = "moving-exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.25},
        },
        image::AdjustmentNode{
            .node_id = "accepted-completion",
            .parameters = image::ImageCompletionAdjustment{.patches = {std::move(patch)}},
        },
    };
    for (const double stops : {-0.25, -0.85}) {
        nodes[0].parameters = image::ExposureAdjustment{.stops = stops};
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu = image::execute_adjustment_nodes(source, nodes);
        const auto gpu = preparation.session->render(nodes, plan, true);
        double maximum_error = 0.0;
        const bool matched = gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
                             && linear_close(
                                 *gpu.output->analyzed_linear, cpu, maximum_error, 2.0e-4
                             );
        if (!matched) {
            std::cerr << "AI completion warm parity: stops=" << stops
                      << " diagnostic=" << gpu.diagnostic
                      << " maximum_error=" << maximum_error << '\n';
        }
        expect(
            matched,
            "changing exposure with accepted completion remains resident and matches CPU"
        );
    }
    const auto stats = preparation.session->stats();
    expect(
        stats.source_upload_count == 1U && stats.completed_render_count == 2U
            && stats.resource_cache_hit_count > 0U,
        "two completion previews reuse one source and their immutable side resource"
    );
}

void cancellation_is_terminal_without_diagnostic() {
    const auto source = make_random_image(64U, 48U, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    const auto nodes = core_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    std::stop_source cancellation;
    expect(cancellation.request_stop(), "first native stop request succeeds");

    if (preparation.session) {
        const auto before = preparation.session->stats();
        const auto attempt =
            preparation.session->render(nodes, plan, true, cancellation.get_token());
        const auto after = preparation.session->stats();
        expect(
            attempt.status == image::detail::WarmEditGpuSession::RenderStatus::cancelled,
            "pre-cancelled resident render has explicit Cancelled status"
        );
        expect(!attempt.output.has_value(), "cancelled resident render returns no pixels");
        expect(attempt.diagnostic.empty(), "cancelled resident render returns no diagnostic");
        expect(
            after.render_count == before.render_count
                && after.completed_render_count == before.completed_render_count,
            "pre-cancelled resident render does not enter or complete a GPU slot"
        );
    }
}

void presentation_pipeline_failure_is_cached_and_falls_back() {
    const auto source = make_random_image(31U, 19U, false);
    auto first_preparation = image::detail::prepare_warm_edit_gpu_session(source);
    auto second_preparation = image::detail::prepare_warm_edit_gpu_session(source);
    expect(
        first_preparation.session != nullptr && second_preparation.session != nullptr,
        "presentation-pipeline failure does not disable the resident edit session"
    );
    if (!first_preparation.session || !second_preparation.session) {
        return;
    }
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto plan = image::compile_edit_execution_plan(neutral_nodes);
    const image::detail::WarmEditGpuRenderContext context{
        .output_intent = image::detail::WarmEditGpuOutputIntent::metal_presentation_surface,
    };
    const auto before = first_preparation.session->stats();
    const auto first = first_preparation.session->render(neutral_nodes, plan, false, context);
    const auto second = first_preparation.session->render(neutral_nodes, plan, false, context);
    const auto after = first_preparation.session->stats();
    const auto fallback_is_named = [](const auto& attempt) {
        return attempt.output.has_value() && attempt.output->presentation_surface == nullptr
               && !attempt.output->rgb8.empty()
               && attempt.output->presentation_fallback_diagnostic
                      == "test-injected Metal presentation surface pipeline failure";
    };
    expect(
        fallback_is_named(first) && fallback_is_named(second),
        "cached presentation-pipeline failure returns named host RGB8 fallbacks"
    );
    expect(
        image::detail::warm_edit_gpu_presentation_pipeline_compile_attempt_count() == 1U,
        "repeated prewarm and surface requests compile a failed pipeline only once"
    );
    expect(
        after.presentation_surface_request_count == before.presentation_surface_request_count + 2U
            && after.presentation_surface_publish_count == before.presentation_surface_publish_count
            && after.presentation_surface_fallback_count
                   == before.presentation_surface_fallback_count + 2U,
        "failed presentation requests increment fallback telemetry without a publish"
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

void benchmark_resident_backend_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_METAL_BENCHMARK") == nullptr) {
        return;
    }
    for (const auto dimensions : std::array{
             image::Dimensions{1'200U, 800U},
             image::Dimensions{2'048U, 1'365U},
         }) {
        const auto source = make_random_image(dimensions.width, dimensions.height, false);
        auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
        if (!preparation.session) {
            std::cerr << "BENCH warm " << dimensions.width << 'x' << dimensions.height
                      << " unavailable: " << preparation.diagnostic << '\n';
            ++failures;
            continue;
        }
        const auto nodes = core_nodes();
        const auto plan = image::compile_edit_execution_plan(nodes);
        std::uint64_t checksum = 0U;

        // Pay lazy process-global pipeline creation and allocator warmup before timing.
        static_cast<void>(preparation.session->render(nodes, plan, false));
        static_cast<void>(image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        ));

        const std::size_t iterations = dimensions.width >= 2'000U ? 5U : 7U;
        const double cpu = median_milliseconds(iterations, [&]() {
            auto adjusted = image::execute_adjustment_nodes_with_backend(
                source,
                nodes,
                {.full_dimensions = source.dimensions},
                image::AdjustmentBackendMode::cpu
            );
            auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
                adjusted.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
            checksum += displayed.bytes[displayed.bytes.size() / 2U];
        });
        const double staged_metal = median_milliseconds(iterations, [&]() {
            auto adjusted = image::execute_adjustment_nodes_with_backend(
                source,
                nodes,
                {.full_dimensions = source.dimensions},
                image::AdjustmentBackendMode::metal
            );
            auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
                adjusted.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::metal
            );
            checksum += displayed.bytes[displayed.bytes.size() / 2U];
        });
        const double resident = median_milliseconds(iterations, [&]() {
            auto rendered = preparation.session->render(nodes, plan, false);
            if (!rendered.output.has_value()) {
                throw std::runtime_error(rendered.diagnostic);
            }
            checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
        });
        const double resident_with_linear = median_milliseconds(iterations, [&]() {
            auto rendered = preparation.session->render(nodes, plan, true);
            if (!rendered.output.has_value() || !rendered.output->analyzed_linear.has_value()) {
                throw std::runtime_error(rendered.diagnostic);
            }
            checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
        });
        const auto stats = preparation.session->stats();
        std::cout << std::fixed << std::setprecision(3) << "BENCH warm " << dimensions.width << 'x'
                  << dimensions.height << " CPU-adjust+display=" << cpu << "ms"
                  << " staged-Metal=" << staged_metal << "ms"
                  << " resident-no-analysis=" << resident << "ms"
                  << " resident+linear-readback=" << resident_with_linear << "ms"
                  << " speedup-vs-CPU=" << cpu / resident << 'x'
                  << " speedup-vs-staged=" << staged_metal / resident << 'x'
                  << " uploads=" << stats.source_upload_count
                  << " buffers=" << stats.gpu_buffer_allocation_count << " checksum=" << checksum
                  << '\n';
    }
}

} // namespace

int run_resident_backend_matches_cpu_oracle() {
    failures = 0;
    host_replay_bytes_are_exact_and_idempotent();
    resident_backend_matches_cpu_oracle();
    accepted_completion_stays_on_the_resident_preview_path();
    return failures;
}

int run_cancellation_contract() {
    failures = 0;
    cancellation_is_terminal_without_diagnostic();
    return failures;
}

int run_presentation_pipeline_failure_contract() {
    failures = 0;
    presentation_pipeline_failure_is_cached_and_falls_back();
    return failures;
}

int run_benchmark_when_requested() {
    failures = 0;
    benchmark_resident_backend_when_requested();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract
